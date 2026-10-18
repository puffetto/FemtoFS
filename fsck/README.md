# The 'fsck' directory

I promised that FemtoFS does not have any fsck and wel... it actually does have one.

This directory contains AI generated code to *test* FemtoFS, nothing here is intended to be used in production or to be exposed to end users.

The LLM working on this directory does *NOT* have access to the code and who develops FemtoFS (along with supporting agents) should NOT look at this directory besides using it as a "Black Box" for testing.

The actual code is responsible for producing images conforming to the specification and to implement the kernel module to mount it (both including internal unit tests).

The code in this directory is responsible to implement tests to verify that an image conforms to the specification and that the kernel module properly handles it.

All this happens in double-bind: the only shared information is the specification.

