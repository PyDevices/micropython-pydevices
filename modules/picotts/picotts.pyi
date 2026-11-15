# SPDX-FileCopyrightText: 2026 Adafruit Industries
# SPDX-FileCopyrightText: 2026 PyDevices
#
# SPDX-License-Identifier: MIT
"""Text to speech on the SVOX Pico engine, with the API of Adafruit's
``adafruit_picotts``: ``tts = picotts.TTS(audio); tts.say("Hello.")``.

It needs about 2.5 MB of RAM, so on a microcontroller it needs PSRAM. Speech is
16 kHz mono, so open the audio output at 16,000 Hz.
"""

from types import TracebackType
from typing import Any, Final

__revision__: Final[str]
"""Which source the firmware was built from."""

class TTS:
    """Speak text through an audio output."""

    def __init__(self, audio: Any, *, voice_dir: str | None = None) -> None:
        """Load the voice and open the speech buffer.

        Args:
            audio: An audio output object with ``play()``, ``stop()`` and ``playing``, such
                as `audiobusio.I2SOut`, or an `audiomixer.MixerVoice` from your own mixer
                whose sample rate is 16,000 Hz.
            voice_dir: The folder with the voice files. By default the first ``picotts/``
                folder on ``sys.path`` that holds them.

        Raises `MemoryError` on a board without enough PSRAM and `OSError` when the voice
        files aren't found.
        """
        ...

    @property
    def sample_rate(self) -> int:
        """Sample rate of the speech output in Hz."""
        ...

    @property
    def speaking(self) -> bool:
        """True while speech is playing or queued."""
        ...

    @property
    def rate(self) -> int:
        """Speaking rate in words per minute. Default 150."""
        ...
    @rate.setter
    def rate(self, value: int) -> None: ...

    @property
    def volume(self) -> float:
        """Volume from 0.0 to 1.0. Default 1.0."""
        ...
    @volume.setter
    def volume(self, value: float) -> None: ...

    @property
    def pitch(self) -> float:
        """Pitch multiplier, 1.0 is normal. Clamped to 0.5 to 2.0."""
        ...
    @pitch.setter
    def pitch(self, value: float) -> None: ...

    @property
    def voice(self) -> str:
        """Voice preset name. See `voices` for the ones available."""
        ...
    @voice.setter
    def voice(self, name: str) -> None: ...

    @property
    def voices(self) -> tuple[str, ...]:
        """Voice preset names available."""
        ...

    def say(self, text: str, *, wait: bool = True, interrupt: bool = False) -> None:
        """Speak ``text``, queued after speech already playing.

        Args:
            text: The text to speak.
            wait: Return when all queued speech ends. If False, return right away and call
                `update` in the main loop.
            interrupt: Stop current speech and clear the queue first.
        """
        ...

    def update(self) -> None:
        """Render and queue the next chunk of speech when the mixer is ready for it."""
        ...

    def stop(self) -> None:
        """Stop speaking and clear the queue."""
        ...

    def deinit(self) -> None:
        """Stop speaking and release the engine and the mixer this object created."""
        ...

    def __enter__(self) -> TTS: ...
    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None: ...
