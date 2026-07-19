"""
============================================================================
bridge.py — PC-side bridge between the ESP32 bot and OpenAI APIs.

Runs on your laptop. Handles TWO kinds of requests from the ESP32:
  1. WAKE_CHECK  — a short audio chunk, asking "was the wake word said?"
                   Answered with a fast Whisper transcription + text match,
                   NO GPT/TTS involved (keeps idle-listening cheap and fast).
  2. CONVERSATION — a full utterance during an active session. Runs the full
                   Whisper -> GPT -> TTS pipeline and returns spoken audio.

USAGE:
    python bridge.py                     # WiFi TCP server mode (default)
    python bridge.py --mode serial --port COM5    # USB Serial mode

SETUP:
    pip install openai pydub pyserial

    Also install ffmpeg (needed to decode TTS mp3 -> PCM) — see BRIDGE_SETUP.md.

    Set your OpenAI API key as an environment variable before running:
        Windows (PowerShell):  $env:OPENAI_API_KEY="sk-..."
        Windows (cmd):         set OPENAI_API_KEY=sk-...
        Mac/Linux:              export OPENAI_API_KEY="sk-..."

DEBUGGING:
    Every stage prints a clearly-labeled line (connection, audio received,
    WAKE/STT/GPT/TTS result, audio sent) so if something breaks, the last
    printed line tells you exactly which stage failed.
============================================================================
"""

import argparse
import io
import os
import re
import socket
import struct
import sys
import time
import wave

# ---------------------------------------------------------------------------
# CONFIG — mirror these to match config.h on the ESP32 side
# ---------------------------------------------------------------------------
TCP_HOST = "0.0.0.0"
TCP_PORT = 8765
SAMPLE_RATE = 16000
SAMPLE_WIDTH_BYTES = 2

BOT_NAME = "Laiba"   # must match BOT_NAME in config.h — used for wake-word matching

SYSTEM_PROMPT = (
    f"You are {BOT_NAME}, a friendly, concise interactive robot at an "
    "engineering expo. Keep answers short (1-3 sentences) and conversational, "
    "since your response will be spoken aloud. Be warm and a little playful."
)

GPT_MODEL = "gpt-4o-mini"
STT_MODEL = "whisper-1"
TTS_MODEL = "tts-1"
TTS_VOICE = "alloy"

# Message type constants — MUST match network.h on the ESP32 exactly.
MSG_TYPE_WAKE_CHECK = 0x01
MSG_TYPE_CONVERSATION = 0x02
RESP_TYPE_WAKE_RESULT = 0x10
RESP_TYPE_AUDIO_REPLY = 0x11

# ---------------------------------------------------------------------------


def log(stage, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] [{stage}] {msg}", flush=True)


def pcm16_to_wav_bytes(pcm_bytes, sample_rate=SAMPLE_RATE):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(SAMPLE_WIDTH_BYTES)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def mp3_bytes_to_pcm16(mp3_bytes, target_rate=SAMPLE_RATE):
    try:
        from pydub import AudioSegment
    except ImportError:
        log("ERROR", "pydub not installed. Run: pip install pydub")
        raise

    try:
        audio = AudioSegment.from_file(io.BytesIO(mp3_bytes), format="mp3")
    except Exception as e:
        log("ERROR", f"MP3 decode failed — is ffmpeg installed and on PATH? ({e})")
        raise

    audio = audio.set_channels(1).set_frame_rate(target_rate).set_sample_width(SAMPLE_WIDTH_BYTES)
    return audio.raw_data


def name_mentioned(text, name):
    """Loose match: strips punctuation/case, checks if the name appears as a whole word.
    Loose on purpose — Whisper transcription of a short 2-second clip can be imperfect,
    and it's better to have an occasional false-positive wake (harmless, session just
    starts and then times out on silence) than to miss real wake attempts."""
    cleaned = re.sub(r"[^\w\s]", "", text.lower())
    return re.search(rf"\b{re.escape(name.lower())}\b", cleaned) is not None


class OpenAIPipeline:
    def __init__(self):
        from openai import OpenAI
        api_key = os.environ.get("OPENAI_API_KEY")
        if not api_key:
            log("FATAL", "OPENAI_API_KEY environment variable not set. See BRIDGE_SETUP.md.")
            sys.exit(1)
        self.client = OpenAI(api_key=api_key)
        self.conversation_history = [{"role": "system", "content": SYSTEM_PROMPT}]

    def check_wake_word(self, pcm_bytes):
        """Fast path: transcribe a short chunk, check if the bot's name was said.
        Returns True/False. Any failure here fails safe (returns False) rather
        than raising, since a wake-check failure should just mean 'try again
        next chunk,' not crash the bridge."""
        try:
            wav_bytes = pcm16_to_wav_bytes(pcm_bytes)
            wav_file = io.BytesIO(wav_bytes)
            wav_file.name = "wake_check.wav"
            transcript = self.client.audio.transcriptions.create(
                model=STT_MODEL,
                file=wav_file,
            )
            text = transcript.text.strip()
            detected = name_mentioned(text, BOT_NAME) if text else False
            log("WAKE", f"heard: '{text}'  -> {'MATCH' if detected else 'no match'}")
            return detected
        except Exception as e:
            log("ERROR", f"Wake check failed: {e}")
            return False

    def process_conversation(self, pcm_bytes):
        """Full pipeline: PCM in -> PCM out. Returns None on any failure."""
        t0 = time.time()

        try:
            wav_bytes = pcm16_to_wav_bytes(pcm_bytes)
            wav_file = io.BytesIO(wav_bytes)
            wav_file.name = "utterance.wav"
            transcript = self.client.audio.transcriptions.create(
                model=STT_MODEL,
                file=wav_file,
            )
            user_text = transcript.text.strip()
            log("STT", f"'{user_text}'  ({time.time()-t0:.2f}s)")
        except Exception as e:
            log("ERROR", f"STT failed: {e}")
            return None

        if not user_text:
            log("STT", "Empty transcript — likely silence/noise. Skipping.")
            return None

        t1 = time.time()
        try:
            self.conversation_history.append({"role": "user", "content": user_text})
            if len(self.conversation_history) > 21:
                self.conversation_history = [self.conversation_history[0]] + self.conversation_history[-20:]

            completion = self.client.chat.completions.create(
                model=GPT_MODEL,
                messages=self.conversation_history,
                max_tokens=150,
            )
            reply_text = completion.choices[0].message.content.strip()
            self.conversation_history.append({"role": "assistant", "content": reply_text})
            log("GPT", f"'{reply_text}'  ({time.time()-t1:.2f}s)")
        except Exception as e:
            log("ERROR", f"GPT call failed: {e}")
            return None

        t2 = time.time()
        try:
            tts_response = self.client.audio.speech.create(
                model=TTS_MODEL,
                voice=TTS_VOICE,
                input=reply_text,
            )
            mp3_bytes = tts_response.content
            pcm_out = mp3_bytes_to_pcm16(mp3_bytes)
            log("TTS", f"{len(pcm_out)} bytes PCM generated  ({time.time()-t2:.2f}s)")
        except Exception as e:
            log("ERROR", f"TTS failed: {e}")
            return None

        log("PIPELINE", f"Total round-trip: {time.time()-t0:.2f}s")
        return pcm_out

    def reset_conversation(self):
        """Call when a session ends (silence timeout / button stop) so the next
        session starts fresh rather than carrying old context indefinitely."""
        self.conversation_history = [self.conversation_history[0]]


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Socket closed while receiving")
        buf += chunk
    return buf


def handle_request(msg_type, payload, pipeline):
    """Shared logic for both WiFi and Serial modes: given a parsed request,
    returns (resp_type, resp_payload_bytes) to send back."""
    if msg_type == MSG_TYPE_WAKE_CHECK:
        log("RECV", f"WAKE_CHECK: {len(payload)} bytes")
        detected = pipeline.check_wake_word(payload)
        return RESP_TYPE_WAKE_RESULT, bytes([0x01 if detected else 0x00])

    elif msg_type == MSG_TYPE_CONVERSATION:
        log("RECV", f"CONVERSATION: {len(payload)} bytes")
        pcm_out = pipeline.process_conversation(payload)
        if pcm_out is None:
            log("SEND", "Sending empty audio reply (pipeline failed upstream)")
            return RESP_TYPE_AUDIO_REPLY, b""
        log("SEND", f"Sending {len(pcm_out)} bytes audio reply")
        return RESP_TYPE_AUDIO_REPLY, pcm_out

    else:
        log("ERROR", f"Unknown message type: {msg_type:#x} — ignoring")
        return RESP_TYPE_AUDIO_REPLY, b""


def handle_wifi_mode(pipeline):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((TCP_HOST, TCP_PORT))
    server.listen(1)
    log("SERVER", f"Listening on {TCP_HOST}:{TCP_PORT} — waiting for ESP32 to connect...")
    log("SERVER", "(Set PC_SERVER_IP in config.h to this machine's LAN IP)")

    while True:
        conn, addr = server.accept()
        log("SERVER", f"ESP32 connected from {addr}")
        try:
            while True:
                msg_type = recv_exact(conn, 1)[0]
                header = recv_exact(conn, 4)
                (byte_len,) = struct.unpack("<I", header)
                payload = recv_exact(conn, byte_len) if byte_len > 0 else b""

                resp_type, resp_payload = handle_request(msg_type, payload, pipeline)

                conn.sendall(bytes([resp_type]))
                conn.sendall(struct.pack("<I", len(resp_payload)))
                if resp_payload:
                    conn.sendall(resp_payload)

        except (ConnectionError, OSError) as e:
            log("SERVER", f"Connection lost ({e}). Waiting for reconnect...")
            pipeline.reset_conversation()
            conn.close()
            continue


def handle_serial_mode(pipeline, port, baud=115200):
    import serial

    ser = serial.Serial(port, baud, timeout=30)
    log("SERVER", f"Listening on serial port {port} @ {baud} baud")

    while True:
        type_byte = ser.read(1)
        if len(type_byte) < 1:
            continue
        msg_type = type_byte[0]

        header = ser.read(4)
        if len(header) < 4:
            continue
        (byte_len,) = struct.unpack("<I", header)
        payload = ser.read(byte_len) if byte_len > 0 else b""

        resp_type, resp_payload = handle_request(msg_type, payload, pipeline)

        ser.write(bytes([resp_type]))
        ser.write(struct.pack("<I", len(resp_payload)))
        if resp_payload:
            ser.write(resp_payload)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["wifi", "serial"], default="wifi")
    parser.add_argument("--port", default=None, help="COM port, required if --mode serial")
    args = parser.parse_args()

    log("BOOT", "Starting bridge...")
    pipeline = OpenAIPipeline()
    log("BOOT", f"OpenAI pipeline ready. Wake word: '{BOT_NAME}'. Model: {GPT_MODEL}, STT: {STT_MODEL}, TTS: {TTS_MODEL}")

    if args.mode == "wifi":
        handle_wifi_mode(pipeline)
    else:
        if not args.port:
            log("FATAL", "--port is required for serial mode (e.g. --port COM5)")
            sys.exit(1)
        handle_serial_mode(pipeline, args.port)


if __name__ == "__main__":
    main()
