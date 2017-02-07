# An interpreter for a Forth-like language

This is an interpreter for a stack-based language, like Forth but without any distinction between immediate and non-immediate words (indeed, part of the point was to see what could be done without the distinction). The language the user deals with is, in concept, a scripting language for the threaded-code generator, with the ability to invoke code already generated. The language is not meant for practical use; it is just for my own experimentation.

There is no dedicated doceumentation, as yet. hello.hfs, a test program, gives some almost-realistic examples, including FizzBuzz. Other than that, there is source code in inner.c (inner for inner interpreter -- a Forth term) and core.hfs, with automated tests in test.hfs.

No floating point support, as yet. Also, support for signed numbers is weak. It allows unstructured code (GOTOs and labels, in effect) as well as recursion.

This program is POSIX-dependent and possibly Linux-dependent. If your kernel's virtual memory manager is not "lazy" in a similar manner to Linux's (which would probably be stupid, but what do I know?) then this program will claim a lot of your physical memory (about 1 part in 16, the amount of virtual memory usually claimed); otherwise, just a few MiBs.

For now (and in case anyone cares), this code is licensed under the **Affero General Public Licence version 3 or later (AGPL-3.0+)**.
