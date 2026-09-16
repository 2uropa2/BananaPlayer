"""Exercise production C++ code with generated media and bounded subprocesses.

Uses only Python's standard library; ffmpeg supplies synthetic test fixtures.
Every run gets its own directory. No personal media or settings are used.
"""
import argparse
import datetime
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import wave


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--label', default='after')
    parser.add_argument('--ffmpeg', default=shutil.which('ffmpeg'))
    parser.add_argument('--cases', default='snapshot-utf8,export-original,playback,network-close,thumbnail-close')
    parser.add_argument('--allow-failures', action='store_true', help='Record baseline failures without passing them as verification')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output = root / 'build' / 'verification' / f'{args.label}-{stamp}'
    output.mkdir(parents=True)
    if not args.ffmpeg:
        parser.error('ffmpeg is required to generate synthetic fixtures; use --ffmpeg PATH')
    fixture = output / 'fixture.mkv'
    creationflags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    subprocess.run([args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y',
                    '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
                    '-t', '3', '-c:v', 'mpeg4', '-g', '10', '-c:a', 'pcm_s16le',
                    str(fixture)], check=True, timeout=30, creationflags=creationflags)
    wav_path = output / 'stream.wav'
    with wave.open(str(wav_path), 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(48000)
        wav.writeframes(b'\0\0' * 48000 * 60)
    stream = wav_path.read_bytes()
    release = threading.Event()

    class StalledServer(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            try:
                if self.path == '/promo.json':
                    import time
                    deadline = time.monotonic() + 5
                    while not (output / 'promo-ready').exists() and time.monotonic() < deadline:
                        if release.wait(0.01):
                            return
                    body = b'{"headline":"new banner"}'
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                self.send_response(200)
                self.send_header('Content-Type', 'audio/wav')
                self.send_header('Content-Length', str(len(stream)))
                self.end_headers()
                if self.path != '/never-start':
                    self.wfile.write(stream[:512 * 1024])
                    self.wfile.flush()
                release.wait(15)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), StalledServer)
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    results = []
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', BP_NO_HW='1')
    env['BP_TEST_PROMO_URL'] = f'http://127.0.0.1:{server.server_port}/promo.json'
    env.pop('BP_PROBE_TRACE', None)
    try:
        for case in args.cases.split(','):
            case_input = fixture
            if case in ('export-original', 'export-alias'):
                case_input = output / (case + '-source.mkv')
                shutil.copy2(fixture, case_input)
            elif case in ('network-close', 'thumbnail-close', 'export-cancel'):
                suffix = '/stream.wav' if case == 'network-close' else '/never-start'
                case_input = f'http://127.0.0.1:{server.server_port}{suffix}'
            command = [str(args.exe.resolve()), case, str(case_input)]
            try:
                completed = subprocess.run(command, capture_output=True, timeout=9,
                                           cwd=output, env=env, creationflags=creationflags)
                log = (completed.stdout + completed.stderr).decode('utf-8', errors='replace')
                result = {'case': case, 'passed': completed.returncode == 0,
                          'exit_code': completed.returncode, 'output': log}
            except subprocess.TimeoutExpired as e:
                log = ((e.stdout or b'') + (e.stderr or b'')).decode('utf-8', errors='replace')
                result = {'case': case, 'passed': False, 'timeout': True, 'output': log}
            (output / f'{case}.log').write_text(log, encoding='utf-8')
            results.append(result)
            print(('PASS' if result['passed'] else 'FAIL') + ' ' + case, flush=True)
    finally:
        release.set()
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)
    report = {'label': args.label, 'exe': str(args.exe.resolve()), 'results': results}
    (output / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'Report: {output / "results.json"}')
    return 0 if all(r['passed'] for r in results) or args.allow_failures else 1


if __name__ == '__main__':
    raise SystemExit(main())
