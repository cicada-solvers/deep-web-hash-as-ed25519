# Failed attempt : Interpret the deep web hash as a ed25519 secret to find its associated v3 onion address

Based on https://github.com/cathugger/mkp224o/

## Modified files
* `main.c` : force arguments
* `worker_slow.inc.h` : changes made to print the resulting onion address, and to prevent additional onions from being generated
* `ed25519/ed25519.h` : function 'ed25519_seckey_expand'. Originally, this function took a random seed and created an ed25519 secret using a 512 bits hashing algorithm. Then, it altered the secret slightly.

All changes are commented.

## Tests available
To select a test, use the associated compile-time define.
Tests :
* Custom hash : use `-DCUSTOM_HASH="DEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEF"`
* Don't alter hash : use `-DDONT_ALTER_HASH`
* Use reverse hash endianness : use `-DREVERSE_ENDIANNESS`

## Compile and generate onion address
```
./autogen.sh
./configure CFLAGS='-DTESTS_DEFINES_HERE -DANOTHER_TEST_DEFINE=42'
make
./mkp224o
```
