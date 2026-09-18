#!/usr/bin/env python3
"""
MIDI to MSX .mus Converter and Search Tool
Searches for MIDI files online, downloads them, and converts them
into 3-channel MSX BASIC PLAY Music Macro Language (.mus) files.
"""

import sys
import os
import re
import struct
import urllib.request
import urllib.parse
import json
import argparse
import subprocess

PITCH_NAMES = ['C', 'C+', 'D', 'D+', 'E', 'F', 'F+', 'G', 'G+', 'A', 'A+', 'B']

def search_bitmidi(query, limit=8):
    """Search BitMidi for MIDI files matching query."""
    url = f"https://bitmidi.com/search?q={urllib.parse.quote(query)}"
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'})
    
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            html = resp.read().decode('utf-8', 'ignore')
    except Exception as e:
        print(f"Network error during search: {e}", file=sys.stderr)
        return []

    # Find all midi slug links: /<name>-mid
    slugs = re.findall(r'href="(/([a-zA-Z0-9_-]+)-mid)"', html)
    results = []
    seen = set()

    for full_path, slug in slugs:
        if slug in seen:
            continue
        seen.add(slug)
        # Format human readable title
        title = slug.replace('-', ' ').title()
        page_url = f"https://bitmidi.com{full_path}"
        results.append({
            'title': title,
            'slug': slug,
            'page_url': page_url
        })
        if len(results) >= limit:
            break

    return results

def get_bitmidi_download_url(page_url):
    """Retrieve the direct .mid download URL from a BitMidi page."""
    req = urllib.request.Request(page_url, headers={'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            html = resp.read().decode('utf-8', 'ignore')
        
        # Look for downloadUrl in JSON or a href
        m = re.search(r'"downloadUrl":"(/uploads/\d+\.mid)"', html)
        if m:
            return f"https://bitmidi.com{m.group(1)}"
        
        m2 = re.search(r'href="(/uploads/[^"]+\.mid)"', html)
        if m2:
            return f"https://bitmidi.com{m2.group(1)}"
    except Exception as e:
        print(f"Error fetching download URL: {e}", file=sys.stderr)
    return None

def download_file(url, target_path):
    """Download a file from a URL to target path."""
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'})
    with urllib.request.urlopen(req, timeout=15) as resp:
        with open(target_path, 'wb') as out:
            out.write(resp.read())

def read_vlq(data, pos):
    """Read variable-length quantity from MIDI data."""
    val = 0
    while True:
        b = data[pos]
        pos += 1
        val = (val << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return val, pos

def parse_midi_file(filepath):
    """Parse standard MIDI file into track structures and events."""
    with open(filepath, 'rb') as f:
        data = f.read()

    if len(data) < 14 or data[:4] != b'MThd':
        raise ValueError("Invalid MIDI file header")

    fmt, num_tracks, division = struct.unpack('>hhh', data[8:14])
    idx = 14

    tracks = []
    tempos = []

    for t_idx in range(num_tracks):
        if idx >= len(data):
            break
        chunk_type = data[idx:idx+4]
        chunk_len = struct.unpack('>I', data[idx+4:idx+8])[0]
        idx += 8
        t_data = data[idx:idx+chunk_len]
        idx += chunk_len

        if chunk_type != b'MTrk':
            continue

        pos, time, running_status = 0, 0, 0
        track_name = f"Track {t_idx + 1}"
        events = [] # (start_time, duration, pitch, channel)
        active = {}

        while pos < len(t_data):
            delta, pos = read_vlq(t_data, pos)
            time += delta
            b = t_data[pos]

            if b & 0x80:
                status = b
                pos += 1
                if status < 0xF0:
                    running_status = status
            else:
                status = running_status

            msg_type = status & 0xF0
            ch = status & 0x0F

            if msg_type == 0x90:
                note, vel = t_data[pos], t_data[pos+1]
                pos += 2
                if vel > 0:
                    active[(note, ch)] = time
                else:
                    if (note, ch) in active:
                        s = active.pop((note, ch))
                        events.append((s, max(1, time - s), note, ch))
            elif msg_type == 0x80:
                note = t_data[pos]
                pos += 2
                if (note, ch) in active:
                    s = active.pop((note, ch))
                    events.append((s, max(1, time - s), note, ch))
            elif msg_type in (0xA0, 0xB0, 0xE0):
                pos += 2
            elif msg_type in (0xC0, 0xD0):
                pos += 1
            elif status == 0xFF:
                meta = t_data[pos]
                pos += 1
                length, pos = read_vlq(t_data, pos)
                m_data = t_data[pos:pos+length]
                pos += length
                if meta == 0x03:
                    name_str = m_data.decode('latin1', 'ignore').strip()
                    if name_str:
                        track_name = name_str
                elif meta == 0x51:
                    us = struct.unpack('>I', b'\x00' + m_data)[0]
                    tempos.append(round(60000000.0 / us))
                elif meta == 0x2F:
                    break
            elif status in (0xF0, 0xF7):
                length, pos = read_vlq(t_data, pos)
                pos += length

        events.sort()
        tracks.append({
            'index': t_idx,
            'name': track_name,
            'events': events,
            'channels': sorted(list(set(e[3] for e in events)))
        })

    avg_tempo = round(sum(tempos) / len(tempos)) if tempos else 120
    avg_tempo = max(32, min(255, avg_tempo))

    return division, avg_tempo, tracks

def demux_tracks_by_channel(tracks):
    """If tracks contain multiple MIDI channels (e.g. Type 0 MIDI), split them into distinct virtual tracks."""
    virtual = []
    for t in tracks:
        ch_map = {}
        for ev in t['events']:
            ch_map.setdefault(ev[3], []).append(ev)
        if len(ch_map) <= 1:
            avg_p = sum(e[2] for e in t['events']) / len(t['events']) if t['events'] else 0
            virtual.append({
                'index': len(virtual) + 1,
                'name': t['name'],
                'events': t['events'],
                'channels': t['channels'],
                'avg_pitch': avg_p
            })
        else:
            for ch in sorted(ch_map.keys()):
                evs = ch_map[ch]
                avg_p = sum(e[2] for e in evs) / len(evs) if evs else 0
                virtual.append({
                    'index': len(virtual) + 1,
                    'name': f"{t['name']} (Ch {ch+1})",
                    'events': evs,
                    'channels': [ch],
                    'avg_pitch': avg_p
                })
    return virtual

def select_or_split_tracks(tracks, user_track_indices=None):
    """Select or split MIDI tracks into exactly 3 monophonic voices for MSX."""
    vtracks = demux_tracks_by_channel(tracks)

    # Filter out empty tracks and drum tracks (channel 9 in 0-indexed = MIDI Ch 10)
    valid_tracks = []
    for t in vtracks:
        melodic_events = [e for e in t['events'] if e[3] != 9]
        if melodic_events:
            valid_tracks.append({
                'index': t['index'],
                'name': t['name'],
                'events': melodic_events,
                'avg_pitch': sum(e[2] for e in melodic_events) / len(melodic_events)
            })

    if not valid_tracks:
        for t in vtracks:
            if t['events']:
                valid_tracks.append(t)

    if not valid_tracks:
        return [], [], []

    # If user explicitly specified track indices
    if user_track_indices:
        selected_events = []
        track_map = {t['index']: t['events'] for t in valid_tracks}
        for u_spec in user_track_indices[:3]:
            merged = []
            if isinstance(u_spec, list):
                for u_idx in u_spec:
                    if u_idx in track_map:
                        merged.extend(track_map[u_idx])
                    elif 1 <= u_idx <= len(valid_tracks):
                        merged.extend(valid_tracks[u_idx - 1]['events'])
            else:
                u_idx = u_spec
                if u_idx in track_map:
                    merged.extend(track_map[u_idx])
                elif 1 <= u_idx <= len(valid_tracks):
                    merged.extend(valid_tracks[u_idx - 1]['events'])
            merged.sort()
            selected_events.append(merged)
        while len(selected_events) < 3:
            selected_events.append([])
        return selected_events[0], selected_events[1], selected_events[2]

    # If we have 3 or more distinct melodic tracks
    if len(valid_tracks) >= 3:
        # Sort tracks by average pitch: highest -> Lead, lowest -> Bass, middle -> Harmony
        valid_tracks.sort(key=lambda t: t['avg_pitch'], reverse=True)
        ch1 = valid_tracks[0]['events']  # Highest (Lead)
        ch3 = valid_tracks[-1]['events'] # Lowest (Bass)
        ch2 = valid_tracks[1]['events']  # Middle (Harmony)
        return ch1, ch2, ch3

    # If we have 1 or 2 polyphonic tracks (e.g. piano arrangement)
    all_notes = []
    for t in valid_tracks:
        all_notes.extend(t['events'])
    all_notes.sort()

    return split_polyphony_to_3_voices(all_notes)

def split_polyphony_to_3_voices(notes):
    """Split a stream of polyphonic notes into top, middle, and bottom voice lines."""
    # Find all unique time boundaries
    time_points = set()
    for s, dur, p, ch in notes:
        time_points.add(s)
        time_points.add(s + dur)
    sorted_times = sorted(list(time_points))

    ch1_events = []
    ch2_events = []
    ch3_events = []

    for i in range(len(sorted_times) - 1):
        t_start = sorted_times[i]
        t_end = sorted_times[i+1]
        t_dur = t_end - t_start
        if t_dur <= 0:
            continue

        # Active notes in this interval
        active_pitches = sorted(list(set(
            p for s, dur, p, ch in notes if s <= t_start and (s + dur) >= t_end
        )))

        if not active_pitches:
            continue

        if len(active_pitches) == 1:
            # 1 note active: assign to lead or bass based on pitch
            p = active_pitches[0]
            if p < 50:
                ch3_events.append((t_start, t_dur, p, 0))
            else:
                ch1_events.append((t_start, t_dur, p, 0))
        elif len(active_pitches) == 2:
            # 2 notes active: high to lead, low to bass
            ch1_events.append((t_start, t_dur, active_pitches[1], 0))
            ch3_events.append((t_start, t_dur, active_pitches[0], 0))
        else:
            # 3 or more notes: highest to lead, lowest to bass, middle to harmony
            ch1_events.append((t_start, t_dur, active_pitches[-1], 0))
            ch2_events.append((t_start, t_dur, active_pitches[len(active_pitches)//2], 0))
            ch3_events.append((t_start, t_dur, active_pitches[0], 0))

    # Merge contiguous intervals of the same pitch
    def merge_events(evs):
        if not evs:
            return []
        merged = []
        cur_s, cur_dur, cur_p, _ = evs[0]
        for s, dur, p, _ in evs[1:]:
            if s == cur_s + cur_dur and p == cur_p:
                cur_dur += dur
            else:
                merged.append((cur_s, cur_dur, cur_p))
                cur_s, cur_dur, cur_p = s, dur, p
        merged.append((cur_s, cur_dur, cur_p))
        return merged

    return merge_events(ch1_events), merge_events(ch2_events), merge_events(ch3_events)

def events_to_msx_mml(events, division, grid_ticks=None):
    """Convert a sequence of monophonic note events to MSX MML string."""
    if not events:
        return "R1"

    # Default grid is sixteenth note (division / 4)
    if not grid_ticks or grid_ticks <= 0:
        grid_ticks = max(1, division // 4)

    # Find total ticks
    total_ticks = max(e[0] + e[1] for e in events)
    total_slots = (total_ticks + grid_ticks - 1) // grid_ticks

    # Rasterize events onto quantized grid slots
    grid = [None] * (total_slots + 1)
    for ev in events:
        s = ev[0]
        dur = ev[1]
        p = ev[2]
        start_slot = round(s / float(grid_ticks))
        dur_slots = max(1, round(dur / float(grid_ticks)))
        for slot in range(dur_slots):
            if start_slot + slot < len(grid):
                grid[start_slot + slot] = (p, slot == 0)

    tokens = []
    idx = 0
    cur_octave = 4

    # Map slot counts to MML duration labels (based on sixteenth grid)
    # 1 slot = 16th, 2 slots = 8th, 3 = 8th., 4 = quarter, 6 = quarter., 8 = half, 12 = half., 16 = whole
    def slots_to_duration_tokens(num_slots):
        sub_tokens = []
        rem = num_slots
        while rem > 0:
            if rem >= 16:
                sub_tokens.append(('1', 16))
                rem -= 16
            elif rem >= 12:
                sub_tokens.append(('2.', 12))
                rem -= 12
            elif rem >= 8:
                sub_tokens.append(('2', 8))
                rem -= 8
            elif rem >= 6:
                sub_tokens.append(('4.', 6))
                rem -= 6
            elif rem >= 4:
                sub_tokens.append(('4', 4))
                rem -= 4
            elif rem >= 3:
                sub_tokens.append(('8.', 3))
                rem -= 3
            elif rem >= 2:
                sub_tokens.append(('8', 2))
                rem -= 2
            else:
                sub_tokens.append(('16', 1))
                rem -= 1
        return sub_tokens

    while idx < len(grid):
        if grid[idx] is None:
            # Run of rests
            rest_slots = 0
            while idx < len(grid) and grid[idx] is None:
                rest_slots += 1
                idx += 1
            for dur_str, _ in slots_to_duration_tokens(rest_slots):
                tokens.append(f"R{dur_str}")
        else:
            # Note run
            pitch, _ = grid[idx]
            note_slots = 1
            idx += 1
            while idx < len(grid) and grid[idx] is not None and grid[idx][0] == pitch and not grid[idx][1]:
                note_slots += 1
                idx += 1

            octave = (pitch // 12) - 1
            # Clamp octave to MSX range 0..8
            octave = max(0, min(8, octave))
            note_name = PITCH_NAMES[pitch % 12]

            dur_list = slots_to_duration_tokens(note_slots)
            for i, (dur_str, _) in enumerate(dur_list):
                oct_prefix = ""
                if octave != cur_octave:
                    oct_prefix = f"O{octave} "
                    cur_octave = octave
                tokens.append(f"{oct_prefix}{note_name}{dur_str}")

    return " ".join(tokens)

def convert_midi_to_mus(midi_file, title="MSX Music Track", user_tracks=None, user_tempo=None):
    """Convert MIDI file to MSX 3-channel .mus format string."""
    division, detected_tempo, tracks = parse_midi_file(midi_file)
    tempo = detected_tempo

    if user_tempo:
        if isinstance(user_tempo, str) and user_tempo.endswith('x'):
            try:
                scale = float(user_tempo[:-1])
                tempo = round(detected_tempo * scale)
            except ValueError:
                pass
        else:
            try:
                tempo = int(user_tempo)
            except ValueError:
                pass
        tempo = max(32, min(255, tempo))

    ch1_evs, ch2_evs, ch3_evs = select_or_split_tracks(tracks, user_tracks)

    ch1_mml = events_to_msx_mml(ch1_evs, division)
    ch2_mml = events_to_msx_mml(ch2_evs, division)
    ch3_mml = events_to_msx_mml(ch3_evs, division)

    output = [
        f"# {title}",
        f"# Tempo: {tempo} BPM (detected: {detected_tempo} BPM), 3-Channel MSX PSG (AY-3-8910)",
        f"# Generated by midi2mus.py",
        "",
        f"# Channel 1: Lead Melody",
        f"CH1: T{tempo} V15 {ch1_mml}",
        "",
        f"# Channel 2: Harmony / Counterpoint",
        f"CH2: T{tempo} V11 {ch2_mml}",
        "",
        f"# Channel 3: Bass Line",
        f"CH3: T{tempo} V13 {ch3_mml}",
        ""
    ]

    return "\n".join(output)

def main():
    parser = argparse.ArgumentParser(
        description="Search for MIDI files online, download them, and convert them to MSX .mus format."
    )
    parser.add_argument("query", nargs="?", help="Song title to search, direct MIDI URL, or local .mid file path")
    parser.add_argument("-s", "--search", help="Explicit search query on BitMidi")
    parser.add_argument("-o", "--output", help="Output .mus file path (default: songs/<name>.mus)")
    parser.add_argument("-y", "--yes", action="store_true", help="Automatically pick first result without prompting")
    parser.add_argument("--select", "--pick", type=int, help="Specify result index to select (e.g. --select 2)")
    parser.add_argument("-t", "--tempo", help="Override tempo in BPM (e.g. -t 85) or scale factor (e.g. -t 0.75x, -t 0.5x)")
    parser.add_argument("-p", "--play", action="store_true", help="Play the converted .mus file immediately using msx_play")
    parser.add_argument("--tracks", help="Comma-separated track numbers to use for CH1,CH2,CH3 (e.g. 1,2,3)")
    parser.add_argument("--list-tracks", action="store_true", help="List tracks in the target MIDI file and exit")

    args = parser.parse_args()

    input_source = args.search if args.search else args.query

    if not input_source:
        print("MSX MIDI-to-.mus Converter & Search Tool")
        print("-" * 50)
        input_source = input("Enter song name to search, MIDI URL, or local file: ").strip()
        if not input_source:
            print("No input provided. Exiting.")
            return 1

    if args.tracks:
        user_tracks = []
        for ch_spec in args.tracks.split(','):
            parts = [int(x.strip()) for x in ch_spec.split('+') if x.strip().isdigit()]
            user_tracks.append(parts)
    else:
        user_tracks = None
    temp_midi_file = None
    song_title = "Converted Track"

    # Case 1: Local file
    if os.path.isfile(input_source):
        local_midi = input_source
        song_title = os.path.splitext(os.path.basename(local_midi))[0].replace('_', ' ').title()
    # Case 2: Direct URL
    elif input_source.startswith("http://") or input_source.startswith("https://"):
        url = input_source
        print(f"Downloading MIDI from URL: {url} ...")
        temp_midi_file = "/tmp/downloaded_midi.mid"
        download_file(url, temp_midi_file)
        local_midi = temp_midi_file
        song_title = os.path.splitext(os.path.basename(url.split('?')[0]))[0].replace('_', ' ').title()
    # Case 3: Online Search query
    else:
        print(f"Searching BitMidi for: '{input_source}' ...")
        results = search_bitmidi(input_source)
        if not results:
            print(f"No MIDI files found matching '{input_source}'. Try different keywords.")
            return 1

        print(f"\nFound {len(results)} matching MIDI files:")
        for idx, r in enumerate(results, 1):
            print(f"  [{idx}] {r['title']}")

        if args.select is not None:
            chosen_idx = max(1, min(args.select, len(results)))
        elif args.yes:
            chosen_idx = 1
        else:
            choice = input(f"\nSelect a track [1-{len(results)}] (default: 1): ").strip()
            chosen_idx = int(choice) if choice.isdigit() and 1 <= int(choice) <= len(results) else 1

        selected = results[chosen_idx - 1]
        song_title = selected['title']
        print(f"\nSelected: {song_title}")
        print("Fetching direct download link ...")
        dl_url = get_bitmidi_download_url(selected['page_url'])
        if not dl_url:
            print("Failed to resolve direct download link.", file=sys.stderr)
            return 1

        temp_midi_file = f"/tmp/{selected['slug']}.mid"
        print(f"Downloading {dl_url} ...")
        download_file(dl_url, temp_midi_file)
        local_midi = temp_midi_file

    # If user just wanted to list tracks
    if args.list_tracks:
        div, tempo, tracks = parse_midi_file(local_midi)
        vtracks = demux_tracks_by_channel(tracks)
        print(f"\nMIDI: {song_title}")
        print(f"Division: {div} ticks/quarter, Detected Tempo: {tempo} BPM")
        print(f"Total Tracks / Instrument Parts: {len(vtracks)}")
        for t in vtracks:
            avg_p = sum(e[2] for e in t['events']) / len(t['events']) if t['events'] else 0
            is_drum = (t['channels'] and t['channels'][0] == 9)
            drum_tag = " [DRUMS/PERCUSSION]" if is_drum else ""
            bass_tag = " [BASS]" if (avg_p > 0 and avg_p < 48 and not is_drum) else ""
            lead_tag = " [LEAD]" if (avg_p >= 68 and not is_drum) else ""
            print(f"  Part {t['index']}: '{t['name']}' ({len(t['events'])} notes, avg_pitch={avg_p:.1f}){drum_tag}{bass_tag}{lead_tag}")
        if temp_midi_file and os.path.exists(temp_midi_file):
            os.remove(temp_midi_file)
        return 0

    print(f"Converting '{song_title}' into 3-channel MSX MML ...")
    mus_content = convert_midi_to_mus(local_midi, title=song_title, user_tracks=user_tracks, user_tempo=args.tempo)

    # Determine output file path
    if args.output:
        out_path = args.output
    else:
        os.makedirs("songs", exist_ok=True)
        safe_name = re.sub(r'[^a-zA-Z0-9_]+', '_', song_title.lower()).strip('_')
        out_path = os.path.join("songs", f"{safe_name}.mus")

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(mus_content)

    print(f"\nSuccessfully created MSX song file: {out_path}")

    # Clean up temp file
    if temp_midi_file and os.path.exists(temp_midi_file):
        os.remove(temp_midi_file)

    # Play if requested
    if args.play:
        print(f"Playing {out_path} with msx_play ...")
        subprocess.run(["./msx_play", out_path])

    return 0

if __name__ == "__main__":
    sys.exit(main())
