"""pio device monitor filter: drop wMbus-lib's per-frame chatter.

wMbus-lib prints "wMBus-lib: Processing T1 A frame" for every telegram it sees
(including other people's meters). Decoding/CRC/coding *errors* and our own
[izar]/[mqtt]/[wifi] lines are kept. Enabled via monitor_filters in platformio.ini.
"""

from platformio.public import DeviceMonitorFilterBase

NOISE_PREFIXES = ("wMBus-lib: Processing",)


class IzarQuiet(DeviceMonitorFilterBase):
    NAME = "izar_quiet"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._pending = ""

    @staticmethod
    def _is_noise(line):
        return line.startswith(NOISE_PREFIXES)

    @staticmethod
    def _could_become_noise(partial):
        return any(p.startswith(partial) or partial.startswith(p) for p in NOISE_PREFIXES)

    def rx(self, text):
        data = self._pending + text
        self._pending = ""
        lines = data.split("\n")
        tail = lines.pop()  # text after the last newline (possibly empty)

        out = [line + "\n" for line in lines if not self._is_noise(line.rstrip("\r"))]
        if tail:
            if self._could_become_noise(tail):
                self._pending = tail  # wait for the rest of the line
            else:
                out.append(tail)
        return "".join(out)
