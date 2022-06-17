#!/bin/env python3

# Converts all passed files into <file>.h
# with the contents assuemd to be ascii, encoded in a C string.

# usage
# ./make-cheader.py ./main/certs/buzzer.{crt,key}

import sys
from pathlib import Path

if __name__ == "__main__":
  for i, arg in enumerate(sys.argv[1:]):
    lines = None
    with open(arg) as f:
      lines = f.readlines()
    with open(arg+".h", 'w') as f:
      for line in lines:
        f.write("\"")
        for char in line:
          c = ord(char)
          if c == 0x0A:
            f.write("\\n")
          elif c == 0x0D:
            f.write("\\r")
          elif c < 0x20:
            f.write("\\x{:02x}".format(c))
          elif c < 0x7F:
            f.write(char)
          else:
            f.write("\\x{:02x}".format(c))
        f.write("\"\n")