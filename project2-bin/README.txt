COMP30023 Project 2 2026 - Binaries

This repository contains the closed-source x86_64 and arm64 binaries for the game engine and client runner.
Link them against your server and client implementations to produce server and client executables.

While there shouldn't be any malicious code, we cannot guarantee that there are no supply chain attacks.
Please run the produced executables on your VM and/or Dev Containers.

You may have the repository as a git submodule like the skeleton repository, to avoid committing .a files to your repo.

However, note that the CI and marking environment will not pull any git submodules.
It will provide its own copy of the .a files.
