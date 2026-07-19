"""
============================================================================
bridge.py — PC-side bridge between the ESP32 bot and an AI provider.

THREE provider options (see PROVIDER below):
  "openai_realtime" — RECOMMENDED. OpenAI's Realtime API: one persistent
                       streaming connection handles STT+Chat+TTS together,
                       instead of 3 separate round-trips. Much faster
                       (typically ~1-2s vs 3-6s+ for the old chained
                       approach). Requires a paid OpenAI key.
  "openai"           — Old chained approach (Whisper -> GPT -> TTS as 3
                       separate calls). Kept as a fallback in case the
                       Realtime API has issues — just flip PROVIDER back.
  "gemini"           — Chained approach using Gemini instead (STT/Chat/TTS
                       all via generate_content, 3 separate calls). Useful
                       if only a Gemini key is available.

Runs on your laptop. Handles TWO kinds of requests from the ESP32:
  1. WAKE_CHECK  — a short audio chunk, asking "was the wake word said?"
                   Answered with a fast STT-only check, NO chat/TTS involved
                   (keeps idle-listening cheap and fast).
  2. CONVERSATION — a full utterance during an active session. Runs the full
                   pipeline and returns spoken audio.

USAGE:
    python bridge.py                     # WiFi TCP server mode (default)
    python bridge.py --mode serial --port COM5    # USB Serial mode

SETUP:
    pip install pydub pyserial
    Then EITHER:
      pip install openai        (if PROVIDER = "openai_realtime" or "openai")
      pip install google-genai  (if PROVIDER = "gemini")

    Also install ffmpeg (needed to decode/resample audio) — see BRIDGE_SETUP.md.

    Set the matching API key as an environment variable before running:
        OpenAI:  OPENAI_API_KEY
        Gemini:  GEMINI_API_KEY
    Windows (PowerShell):  $env:GEMINI_API_KEY="..."
    Windows (cmd):         set GEMINI_API_KEY=...
    Mac/Linux:              export GEMINI_API_KEY="..."

DEBUGGING:
    Every stage prints a clearly-labeled line (connection, audio received,
    WAKE/STT/CHAT/TTS result, audio sent) so if something breaks, the last
    printed line tells you exactly which stage failed.
============================================================================
"""

import argparse
import asyncio
import base64
import io
import os
import re
import socket
import struct
import sys
import time
import wave

# ---------------------------------------------------------------------------
# PROVIDER SWITCH — change this ONE line to swap providers.
# "openai_realtime" is the fast option — try this first. If it has problems,
# fall back to "openai" (slower but proven) or "gemini" (if that's what's
# available). Everything downstream (handle_request, wire protocol, ESP32
# firmware) is identical regardless of which one is active.
# ---------------------------------------------------------------------------
PROVIDER = "openai_realtime"   # "openai_realtime" (fast, recommended), "openai", or "gemini"

# ---------------------------------------------------------------------------
# CONFIG — mirror SAMPLE_RATE/BOT_NAME to match config.h on the ESP32 side
# ---------------------------------------------------------------------------
TCP_HOST = "0.0.0.0"
TCP_PORT = 8765
SAMPLE_RATE = 16000          # what the ESP32 sends/expects — both pipelines convert to/from this
SAMPLE_WIDTH_BYTES = 2

BOT_NAME = "Laiba"   # must match BOT_NAME in config.h — used for wake-word matching

SYSTEM_PROMPT = (
    f"You are {BOT_NAME}, a friendly, concise interactive robot at an "
    "engineering expo. Keep answers short (1-3 sentences) and conversational, "
    "since your response will be spoken aloud. Be warm and a little playful."
)

# --- OpenAI Realtime-specific settings (used if PROVIDER == "openai_realtime") ---
OPENAI_REALTIME_MODEL = "gpt-realtime-2"
OPENAI_REALTIME_VOICE = "alloy"

# --- OpenAI chained-pipeline settings (used if PROVIDER == "openai") ---
OPENAI_CHAT_MODEL = "gpt-4o-mini"
OPENAI_STT_MODEL = "whisper-1"
OPENAI_TTS_MODEL = "tts-1"
OPENAI_TTS_VOICE = "alloy"

# --- Gemini-specific settings (used if PROVIDER == "gemini") ---
GEMINI_CHAT_MODEL = "gemini-3.5-flash"        # fast + cheap, good for live demo latency
GEMINI_STT_MODEL = "gemini-3.5-flash"          # same model handles audio-in transcription
GEMINI_TTS_MODEL = "gemini-3.1-flash-tts-preview"
GEMINI_TTS_VOICE = "Kore"
GEMINI_TTS_OUTPUT_RATE = 24000                 # Gemini TTS always outputs 24kHz PCM16 mono

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


def resample_pcm16(pcm_bytes, source_rate, target_rate):
    """Resample raw PCM16 mono bytes from source_rate to target_rate.
    Used for Gemini TTS output, which is always 24kHz — the ESP32 expects
    SAMPLE_RATE_HZ (16kHz per config.h), so this converts before sending."""
    if source_rate == target_rate:
        return pcm_bytes
    try:
        from pydub import AudioSegment
    except ImportError:
        log("ERROR", "pydub not installed. Run: pip install pydub")
        raise

    audio = AudioSegment(
        data=pcm_bytes,
        sample_width=SAMPLE_WIDTH_BYTES,
        frame_rate=source_rate,
        channels=1,
    )
    audio = audio.set_frame_rate(target_rate)
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
                model=OPENAI_STT_MODEL,
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
                model=OPENAI_STT_MODEL,
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
                model=OPENAI_CHAT_MODEL,
                messages=self.conversation_history,
                max_tokens=150,
            )
            reply_text = completion.choices[0].message.content.strip()
            self.conversation_history.append({"role": "assistant", "content": reply_text})
            log("CHAT", f"'{reply_text}'  ({time.time()-t1:.2f}s)")
        except Exception as e:
            log("ERROR", f"Chat call failed: {e}")
            return None

        t2 = time.time()
        try:
            tts_response = self.client.audio.speech.create(
                model=OPENAI_TTS_MODEL,
                voice=OPENAI_TTS_VOICE,
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


class GeminiPipeline:
    """Same interface as OpenAIPipeline (check_wake_word, process_conversation,
    reset_conversation) — handle_request() and everything else downstream
    doesn't need to know or care which provider is actually running."""

    def __init__(self):
        from google import genai
        api_key = os.environ.get("GEMINI_API_KEY")
        if not api_key:
            log("FATAL", "GEMINI_API_KEY environment variable not set. See BRIDGE_SETUP.md.")
            sys.exit(1)
        self.client = genai.Client(api_key=api_key)
        # Gemini's generate_content is stateless per-call (no server-side thread),
        # so conversation history is tracked here as plain text turns and
        # replayed as context on every call — same end effect as OpenAI's
        # message list, just assembled into a single prompt string per call.
        self.history_text = ""

    def _transcribe(self, pcm_bytes):
        from google.genai import types
        wav_bytes = pcm16_to_wav_bytes(pcm_bytes)
        response = self.client.models.generate_content(
            model=GEMINI_STT_MODEL,
            contents=[
                "Transcribe this audio clip. Reply with ONLY the transcription "
                "text, no preamble, no quotes. If there is no speech (silence "
                "or noise only), reply with an empty string.",
                types.Part.from_bytes(data=wav_bytes, mime_type="audio/wav"),
            ],
        )
        return (response.text or "").strip()

    def check_wake_word(self, pcm_bytes):
        """Fast path: transcribe a short chunk, check if the bot's name was said.
        Returns True/False. Any failure here fails safe (returns False) rather
        than raising, since a wake-check failure should just mean 'try again
        next chunk,' not crash the bridge."""
        try:
            text = self._transcribe(pcm_bytes)
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
            user_text = self._transcribe(pcm_bytes)
            log("STT", f"'{user_text}'  ({time.time()-t0:.2f}s)")
        except Exception as e:
            log("ERROR", f"STT failed: {e}")
            return None

        if not user_text:
            log("STT", "Empty transcript — likely silence/noise. Skipping.")
            return None

        t1 = time.time()
        try:
            # Build a single prompt: system instruction + running history + new turn.
            # Trim history to keep prompt size reasonable across a long expo day.
            self.history_text += f"\nUser: {user_text}"
            history_lines = self.history_text.strip().split("\n")
            if len(history_lines) > 40:
                history_lines = history_lines[-40:]
            self.history_text = "\n".join(history_lines)

            full_prompt = f"{SYSTEM_PROMPT}\n\nConversation so far:\n{self.history_text}\n\nAssistant:"

            response = self.client.models.generate_content(
                model=GEMINI_CHAT_MODEL,
                contents=full_prompt,
            )
            reply_text = (response.text or "").strip()
            self.history_text += f"\nAssistant: {reply_text}"
            log("CHAT", f"'{reply_text}'  ({time.time()-t1:.2f}s)")
        except Exception as e:
            log("ERROR", f"Chat call failed: {e}")
            return None

        if not reply_text:
            log("ERROR", "Empty chat reply — skipping TTS")
            return None

        t2 = time.time()
        try:
            from google.genai import types
            response = self.client.models.generate_content(
                model=GEMINI_TTS_MODEL,
                contents=reply_text,
                config=types.GenerateContentConfig(
                    response_modalities=["AUDIO"],
                    speech_config=types.SpeechConfig(
                        voice_config=types.VoiceConfig(
                            prebuilt_voice_config=types.PrebuiltVoiceConfig(
                                voice_name=GEMINI_TTS_VOICE,
                            )
                        )
                    ),
                ),
            )
            raw_pcm_24k = response.candidates[0].content.parts[0].inline_data.data
            pcm_out = resample_pcm16(raw_pcm_24k, GEMINI_TTS_OUTPUT_RATE, SAMPLE_RATE)
            log("TTS", f"{len(pcm_out)} bytes PCM generated  ({time.time()-t2:.2f}s)")
        except Exception as e:
            log("ERROR", f"TTS failed: {e}")
            return None

        log("PIPELINE", f"Total round-trip: {time.time()-t0:.2f}s")
        return pcm_out

    def reset_conversation(self):
        """Call when a session ends (silence timeout / button stop) so the next
        session starts fresh rather than carrying old context indefinitely."""
        self.history_text = ""


class OpenAIRealtimePipeline:
    """Uses OpenAI's Realtime API: ONE persistent streaming connection handles
    STT + Chat + TTS together, instead of 3 separate round-trip calls. This
    is the fast option — typically ~1-2s total vs 3-6s+ for the chained
    OpenAIPipeline/GeminiPipeline approach, because there's no waiting for
    a full transcript before starting the chat call, then waiting for a full
    chat reply before starting TTS — it's one continuous stream.

    Same public interface as OpenAIPipeline/GeminiPipeline (check_wake_word,
    process_conversation, reset_conversation) — handle_request() doesn't
    need to know or care that this one works differently internally.

    Internally async (the Realtime API is WebSocket-based), but wrapped so
    the rest of bridge.py — which is plain synchronous socket code — can
    call it exactly like the other two pipelines. A single background
    asyncio event loop is created once and reused for every call, which
    avoids the overhead of spinning up a new loop (and a new WebSocket
    connection) on every single turn.
    """

    def __init__(self):
        from openai import AsyncOpenAI
        api_key = os.environ.get("OPENAI_API_KEY")
        if not api_key:
            log("FATAL", "OPENAI_API_KEY environment variable not set. See BRIDGE_SETUP.md.")
            sys.exit(1)
        self.client = AsyncOpenAI(api_key=api_key)
        self.conversation_history = []  # list of {"role":..., "content":...} for context replay

        # Dedicated background event loop, running in its own thread, reused
        # across every call — this is what avoids reconnect overhead per turn.
        import threading
        self._loop = asyncio.new_event_loop()
        self._loop_thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._loop_thread.start()

    def _run_async(self, coro, timeout=30):
        """Run an async coroutine on the background loop from synchronous code."""
        future = asyncio.run_coroutine_threadsafe(coro, self._loop)
        return future.result(timeout=timeout)

    async def _realtime_turn(self, pcm_bytes):
        """One full turn: send audio in, get transcript + audio reply out.
        Opens a fresh connection per turn — simpler and more robust than trying
        to keep one WebSocket alive across the gaps between conversation turns
        (which can be many seconds/minutes apart and risk idle timeouts).

        Always requests an audio reply (the GA API doesn't have a clean
        text-only toggle in this session shape) — for wake-checks the caller
        just discards the audio bytes and looks only at the transcript. The
        cost of a short 2-second wake-check clip generating an unused audio
        reply is negligible.

        IMPORTANT: the GA Realtime API's PCM format is fixed at 24kHz — it is
        NOT configurable to 16kHz like some other APIs. So audio going in is
        upsampled from your ESP32's 16kHz to 24kHz, and audio coming back is
        downsampled from 24kHz to 16kHz before being returned (matching what
        the ESP32 expects, same as the Gemini pipeline's resample step)."""
        pcm_24k_in = resample_pcm16(pcm_bytes, SAMPLE_RATE, 24000)
        audio_b64 = base64.b64encode(pcm_24k_in).decode("ascii")

        user_transcript = ""
        reply_transcript = ""
        audio_chunks = []

        async with self.client.realtime.connect(model=OPENAI_REALTIME_MODEL) as connection:
            # GA API shape: type must be "realtime", audio config is nested
            # under audio.input / audio.output (NOT a top-level
            # "output_modalities" list — that was a beta-only field).
            session_config = {
                "type": "realtime",
                "instructions": SYSTEM_PROMPT,
                "audio": {
                    "input": {
                        "format": {"type": "audio/pcm", "rate": 24000},
                        "transcription": {"model": "gpt-realtime-whisper"},
                    },
                    "output": {
                        "format": {"type": "audio/pcm", "rate": 24000},
                        "voice": OPENAI_REALTIME_VOICE,
                    },
                },
            }
            await connection.session.update(session=session_config)

            # Replay prior turns as context so the conversation has memory
            # across separate connections (each turn opens a fresh WebSocket).
            for turn in self.conversation_history[-20:]:
                await connection.conversation.item.create(
                    item={
                        "type": "message",
                        "role": turn["role"],
                        "content": [{
                            "type": "input_text" if turn["role"] == "user" else "output_text",
                            "text": turn["content"],
                        }],
                    }
                )

            await connection.input_audio_buffer.append(audio=audio_b64)
            await connection.input_audio_buffer.commit()
            await connection.response.create()

            async for event in connection:
                etype = getattr(event, "type", "")

                if etype == "conversation.item.input_audio_transcription.completed":
                    user_transcript = getattr(event, "transcript", "") or ""
                elif etype == "response.output_audio_transcript.delta":
                    reply_transcript += getattr(event, "delta", "") or ""
                elif etype == "response.output_audio.delta":
                    chunk_b64 = getattr(event, "delta", "")
                    if chunk_b64:
                        audio_chunks.append(base64.b64decode(chunk_b64))
                elif etype == "response.done":
                    break
                elif etype == "error":
                    err = getattr(event, "error", None)
                    msg = getattr(err, "message", str(err)) if err else "unknown error"
                    log("ERROR", f"Realtime API error event: {msg}")
                    break

        pcm_24k_out = b"".join(audio_chunks) if audio_chunks else None
        pcm_reply = resample_pcm16(pcm_24k_out, 24000, SAMPLE_RATE) if pcm_24k_out else None
        return user_transcript.strip(), reply_transcript.strip(), pcm_reply

    def check_wake_word(self, pcm_bytes):
        """Fast path: run a turn, check if the bot's name was said in the
        transcript. Fails safe (returns False) rather than raising, since a
        wake-check failure should just mean 'try again next chunk,' not
        crash the bridge."""
        try:
            user_text, _, _ = self._run_async(
                self._realtime_turn(pcm_bytes), timeout=10
            )
            detected = name_mentioned(user_text, BOT_NAME) if user_text else False
            log("WAKE", f"heard: '{user_text}'  -> {'MATCH' if detected else 'no match'}")
            return detected
        except Exception as e:
            log("ERROR", f"Wake check failed: {e}")
            return False

    def process_conversation(self, pcm_bytes):
        """Full pipeline in ONE streamed round-trip: PCM in -> PCM out."""
        t0 = time.time()
        try:
            user_text, reply_text, pcm_reply = self._run_async(
                self._realtime_turn(pcm_bytes), timeout=30
            )
        except Exception as e:
            log("ERROR", f"Realtime turn failed: {e}")
            return None

        log("STT", f"'{user_text}'")
        if not user_text:
            log("STT", "Empty transcript — likely silence/noise. Skipping.")
            return None

        log("CHAT", f"'{reply_text}'")
        if not pcm_reply:
            log("ERROR", "No audio in reply — skipping playback")
            return None

        self.conversation_history.append({"role": "user", "content": user_text})
        self.conversation_history.append({"role": "assistant", "content": reply_text})
        if len(self.conversation_history) > 40:
            self.conversation_history = self.conversation_history[-40:]

        log("PIPELINE", f"Total round-trip: {time.time()-t0:.2f}s  ({len(pcm_reply)} bytes audio)")
        return pcm_reply

    def reset_conversation(self):
        """Call when a session ends (silence timeout / button stop) so the next
        session starts fresh rather than carrying old context indefinitely."""
        self.conversation_history = []


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

    log("BOOT", f"Starting bridge... (provider: {PROVIDER})")

    if PROVIDER == "openai_realtime":
        pipeline = OpenAIRealtimePipeline()
        log("BOOT", f"OpenAI Realtime pipeline ready. Wake word: '{BOT_NAME}'. "
                     f"Model: {OPENAI_REALTIME_MODEL} (single-stream STT+Chat+TTS, fast path)")
    elif PROVIDER == "openai":
        pipeline = OpenAIPipeline()
        log("BOOT", f"OpenAI pipeline ready. Wake word: '{BOT_NAME}'. "
                     f"Chat: {OPENAI_CHAT_MODEL}, STT: {OPENAI_STT_MODEL}, TTS: {OPENAI_TTS_MODEL}")
    elif PROVIDER == "gemini":
        pipeline = GeminiPipeline()
        log("BOOT", f"Gemini pipeline ready. Wake word: '{BOT_NAME}'. "
                     f"Chat: {GEMINI_CHAT_MODEL}, STT: {GEMINI_STT_MODEL}, TTS: {GEMINI_TTS_MODEL}")
    else:
        log("FATAL", f"Unknown PROVIDER: '{PROVIDER}' — must be 'openai_realtime', 'openai', or 'gemini'")
        sys.exit(1)

    if args.mode == "wifi":
        handle_wifi_mode(pipeline)
    else:
        if not args.port:
            log("FATAL", "--port is required for serial mode (e.g. --port COM5)")
            sys.exit(1)
        handle_serial_mode(pipeline, args.port)


if __name__ == "__main__":
    main()
