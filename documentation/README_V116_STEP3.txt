ATOM CYD V116 - AtoMMC3E Step 3 read-only filesystem
======================================================

Based directly on the tested/accepted V115.

Unchanged
---------
- 8-bank #A000 utility ROM system:
    0 P-Charme
    1 ASBK-I v1.00
    2 DISATOM
    3 SALFAA 2.6
    4 GAGS 2.3
    5 PC*BOX v1.02
    6 TOOLBUG
    7 AXR-1
- ATOMMC2 V3.02E mapped at #E000-#EFFF
- patched Atom Kernel at #F000-#FFFF
- existing keyboard, display, cassette, direct SD filesystem and screenshots

Step 3 additions
----------------
The genuine AtoMMC #E000 ROM now drives a read-only SD backend through
the emulated #B400-#B404 controller registers.

Implemented:
- *CAT
- *INFO <file>
- *LOAD <file>
- *RUN <file>
- *CWD <directory> (safe read-only navigation)
- relative paths and basic * / ? wildcards

The AtoMMC backend is rooted at:
    /ATOM/TAPES

Compatibility with the existing emulator SD layout:
- physical files named NAME.atm are exposed to AtoMMC as NAME
- *CAT displays NAME rather than NAME.atm
- *LOAD NAME transparently opens NAME.atm
- directories are displayed in the AtoMMC <DIRNAME> form

Still deliberately disabled
----------------------------
SAVE/write, DELETE, RENAME, MKDIR, RMDIR and other mutating commands.
They return write-protected/not-implemented style errors rather than hanging.

Important test sequence
-----------------------
1. Boot: should still show ACORN ATOM + ATOMMC2.
2. *CAT
3. *INFO ASSEMBLER
4. *LOAD ASSEMBLER
5. optionally *RUN REVERSI or another known program
6. *CWD Games01 then *CAT, and *CWD .. to return.

This is a staged implementation. Keep V115 as the rollback milestone.
