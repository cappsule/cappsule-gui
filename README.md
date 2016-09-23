# Cappsule's GUI

This repository is part of [Cappsule](https://github.com/cappsule), and contains
the GUI part. Please refer to the
[documentation](https://github.com/cappsule/cappsule-doc/) for more information.



## Overview

Fork of [Qubes OS GUI protocol](https://www.qubes-os.org/doc/gui/)
implementation. There's nothing of interest here. The communication mechanism
between userland and the hypervisor has been modified to use Cappsule's xchan.



## Architecture

- `agent-linux/`: fork of
  [Qubes OS agent linux](https://github.com/QubesOS/qubes-gui-agent-linux)
- `common/`: fork of
  [Qubes OS GUI common](https://github.com/QubesOS/qubes-gui-common)
- `daemon/`: fork of
  [Qubes OS GUI daemon](https://github.com/QubesOS/qubes-gui-daemon)
- `metacity/`: patch adding colored borders to the window manager
- `qubes-drv/`: fork of
  [Qubes OS agent linux](https://github.com/QubesOS/qubes-gui-agent-linux)

The [hypervisor](https://github.com/cappsule/cappsule-hypervisor/) and
[userland](https://github.com/cappsule/cappsule-userland/) repositories are
required to build the project because they contain header files.
