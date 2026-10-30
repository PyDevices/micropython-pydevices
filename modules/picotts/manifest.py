# picotts: text to speech on the SVOX Pico engine, the API of Adafruit's
# adafruit_picotts. The engine is C (_picotts); the TTS class is frozen Python.
# Needs PSRAM on a microcontroller: about 2.5 MB for the engine and voice.
c_module(".")
module("picotts.py", base_path="py")
