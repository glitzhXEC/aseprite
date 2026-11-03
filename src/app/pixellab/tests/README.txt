Synthetic offline test; no credentials, no live API, no private user fixtures.
Requires Linux/macOS C++17 toolchain, Python 3 and Pillow for development only.
Example Linux (run in this tests folder, after CMake configure):
  export DEPS=/absolute/path/to/build/src/app/pixellab/deps
  mkdir -p obj
  for f in miniz miniz_tdef miniz_tinfl miniz_zip; do gcc -O1 -fPIC -c "$DEPS/vendor/miniz/$f.c" -o obj/$f.o; done
  g++ -std=c++17 -O1 -fPIC -shared -pthread -I"$DEPS" mock_adapter.cpp obj/*.o -o libmock.so
  python3 test_core.py
  PIXELLAB_MOCK_DIRECT=1 python3 test_core.py
The Linux and Windows clients do not require Python at runtime.

Checks original pixel snapshots before writes, rejecting missing/stale data
and checking that a later-direction conflict blocks all writes. This simulated
preflight is not an atomic PixelLab transaction.
