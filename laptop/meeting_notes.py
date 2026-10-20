"""Phase 3: turn meeting recordings into a transcript and a summary.

    python meeting_notes.py REC_0001.WAV          # one file
    python meeting_notes.py recordings/           # every WAV in a folder
    python meeting_notes.py --device 192.168.1.42 # download new recordings from the recorder first

For each REC_0001.WAV you get:
    REC_0001.txt  the transcript, with timestamps (made on your laptop by Whisper, free)
    REC_0001.md   the meeting notes: summary, decisions, action items (written by Claude)

Files that already have notes are skipped, so it's safe to run again and again.
"""

import argparse
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

import anthropic
from faster_whisper import WhisperModel

CLAUDE_MODEL = "claude-opus-5-5"

NOTES_PROMPT = """Below is an automatic transcript of a meeting recording. \
Write meeting notes in Markdown, in the same language as the meeting, with these sections:

## Summary
A few sentences: what the meeting was about and where it ended up.

## Decisions
What was agreed. Write "None" if nothing was decided.

## Action items
A checklist ("- [ ] ..."): who does what, and by when if a date was mentioned.
Write "None" if there are none.

## Open questions
Things raised but not settled. Leave the section out if there are none.

The transcript was made by speech recognition, so expect misheard words and no speaker names. \
Fix obvious mishearings from context. Where you can't tell who said something, don't guess a name. \
Start directly with "## Summary".

<transcript>
{transcript}
</transcript>"""


def timestamp(seconds):
    h, rest = divmod(int(seconds), 3600)
    m, s = divmod(rest, 60)
    return f"{h:02d}:{m:02d}:{s:02d}"


def download_from_device(address, folder):
    """Download the recorder's WAV files that aren't in `folder` yet."""
    base = "http://" + address.removeprefix("http://").rstrip("/")
    print(f"Connecting to the recorder at {base} ...")
    try:
        page = urllib.request.urlopen(base + "/", timeout=10).read().decode()
    except OSError as e:
        sys.exit(f"Could not reach the recorder: {e}\n"
                 "Check the address in its Serial Monitor and that the laptop is on the same Wi-Fi.")

    # The recorder's web page lists each file as: <a href='/download?f=/REC_0001.WAV'>REC_0001.WAV</a> (1234 KB)
    listed = re.findall(r"href='/download\?f=/([^']+\.WAV)'>[^<]*</a> \((\d+) KB\)", page, re.IGNORECASE)
    if not listed:
        print("No recordings on the card.")
    folder.mkdir(parents=True, exist_ok=True)

    for name, size_kb in listed:
        target = folder / name
        # File numbers start again at REC_0001 after "Delete all files", so the same name can be
        # a different recording. Same name and same size means we already have it.
        n = 2
        while target.exists():
            if target.stat().st_size // 1024 == int(size_kb):
                break
            target = folder / f"{Path(name).stem}-{n}{Path(name).suffix}"
            n += 1
        if target.exists():
            continue

        print(f"Downloading {name} ({int(size_kb) / 1024:.1f} MB) ...")
        partial = target.with_name(target.name + ".part")
        try:
            with urllib.request.urlopen(f"{base}/download?f=/{name}", timeout=30) as r, open(partial, "wb") as f:
                while chunk := r.read(64 * 1024):
                    f.write(chunk)
        except urllib.error.HTTPError as e:
            partial.unlink(missing_ok=True)
            if e.code == 409:
                sys.exit("The recorder is recording right now. Stop the recording and try again.")
            sys.exit(f"Download of {name} failed: {e}")
        except OSError as e:
            partial.unlink(missing_ok=True)
            sys.exit(f"Download of {name} failed: {e}")
        partial.rename(target)
        print(f"  saved as {target}")


def transcribe(wav, whisper, language):
    print(f"Transcribing {wav.name} (this takes a while; the first run also downloads the Whisper model) ...")
    segments, info = whisper.transcribe(str(wav), language=language, vad_filter=True)
    print(f"  language: {info.language}, length: {timestamp(info.duration)}")
    lines = []
    for seg in segments:  # segments arrive one by one while Whisper works
        line = f"[{timestamp(seg.start)}] {seg.text.strip()}"
        print("  " + line)
        lines.append(line)
    return "\n".join(lines)


def write_notes(transcript, client):
    print("Asking Claude for the meeting notes ...")
    with client.beta.messages.stream(
        model=CLAUDE_MODEL,
        max_tokens=64000,
        output_config={"effort": "medium"},
        # If Claude's safety check declines, the request is retried on another model automatically.
        betas=["server-side-fallback-2026-07-01"],
        fallbacks="default",
        messages=[{"role": "user", "content": NOTES_PROMPT.format(transcript=transcript)}],
    ) as stream:
        response = stream.get_final_message()
    if response.stop_reason == "refusal":
        raise RuntimeError("Claude declined to summarize this recording.")
    return "".join(block.text for block in response.content if block.type == "text").strip()


def main():
    parser = argparse.ArgumentParser(description="Turn meeting recordings into transcripts and notes.")
    parser.add_argument("paths", nargs="*", type=Path, help="WAV files or folders (default: recordings/)")
    parser.add_argument("--device", metavar="ADDRESS",
                        help="recorder address, e.g. 192.168.1.42: download its new recordings first")
    parser.add_argument("--language", help="language code such as en or tr (default: detect automatically)")
    parser.add_argument("--model", default="small",
                        help="Whisper model: tiny, base, small, medium, large-v3 (bigger = better and slower)")
    parser.add_argument("--no-summary", action="store_true", help="only make transcripts, don't call Claude")
    args = parser.parse_args()

    paths = args.paths or [Path("recordings")]
    if args.device:
        download_from_device(args.device, paths[0] if paths[0].suffix == "" else paths[0].parent)

    wavs = []
    for p in paths:
        if p.is_dir():
            wavs += sorted(f for f in p.iterdir() if f.suffix.lower() == ".wav")
        elif p.is_file():
            wavs.append(p)
        else:
            print(f"Not found: {p}")
    todo = [w for w in wavs if not w.with_suffix(".md").exists()
            and not (args.no_summary and w.with_suffix(".txt").exists())]
    if not todo:
        print("Nothing new to do.")
        return

    whisper = None
    client = None if args.no_summary else anthropic.Anthropic()
    for wav in todo:
        txt = wav.with_suffix(".txt")
        if txt.exists():  # transcribed on an earlier run, e.g. one where the summary step failed
            transcript = txt.read_text(encoding="utf-8")
        else:
            if whisper is None:
                whisper = WhisperModel(args.model, device="cpu", compute_type="int8")
            transcript = transcribe(wav, whisper, args.language)
            txt.write_text(transcript + "\n", encoding="utf-8")
            print(f"  transcript saved: {txt}")

        if args.no_summary:
            continue
        if not transcript.strip():
            print(f"  {wav.name}: no speech found, skipping the summary.")
            continue
        try:
            notes = write_notes(transcript, client)
        except (anthropic.AuthenticationError, TypeError) as e:
            if isinstance(e, TypeError) and "authentication" not in str(e):
                raise  # a real bug, not a missing key
            sys.exit("Claude API key missing or wrong. Set ANTHROPIC_API_KEY (see BUILD_GUIDE.md, phase 3),\n"
                     "or run with --no-summary to only make transcripts.")
        except (anthropic.APIError, RuntimeError) as e:
            print(f"  {wav.name}: summary failed ({e}). The transcript is saved; run again to retry.")
            continue
        md = wav.with_suffix(".md")
        md.write_text(f"# Meeting notes: {wav.stem}\n\n{notes}\n\n---\n\n## Transcript\n\n{transcript}\n",
                      encoding="utf-8")
        print(f"  notes saved: {md}")


if __name__ == "__main__":
    main()
