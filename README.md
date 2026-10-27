# clipsd

A small resident driver that wraps CLIPS 6.4.2 behind **JSON Lines over stdio**.

One process holds one CLIPS environment. It reads one JSON command per line from standard input and writes one JSON response per line to standard output. This makes stateful sessions easy (assert facts, `run`, assert more facts, `run` again), and any front end that can spawn a process and talk over stdin/stdout can use it: a CLI, an MCP tool, Pharo/GToolkit, and so on.

```
CLI / MCP tool / Pharo-GToolkit
        │  JSON Lines (stdin / stdout)
        ▼
     clipsd ──── libclips.a (CLIPS 6.4.2)
```

## Features

- Facts, rules, and the agenda are returned as structured JSON.
- `watch` output (fact assertions/retractions, activations, rule firings) is converted into an `events` array.
- Ordinary CLIPS output (such as `printout`) is separated into an `out` field, so it never pollutes the protocol stream.
- The only dependencies are CLIPS and [cJSON](https://github.com/DaveGamble/cJSON) (bundled).

## Prerequisites

The CLIPS source code is **not** included in this repository. Build CLIPS separately and place its headers and library under `CLIPS_DIR` (default: `$HOME/clips`).

1. Download `clips_core_source_642.tar.gz` from the [CLIPS project on SourceForge](https://sourceforge.net/projects/clipsrules/files/CLIPS/6.4.2/) and extract it.
2. Run `make` in the extracted `core` directory to build `clips` and `libclips.a`.
3. Copy the headers and the library into `CLIPS_DIR`:

```bash
mkdir -p ~/clips/bin ~/clips/lib ~/clips/include
cp core/clips      ~/clips/bin/
cp core/libclips.a ~/clips/lib/
cp core/*.h        ~/clips/include/
```

You need `gcc` and `make`. This has been tested with CLIPS 6.4.2 on WSL2 (Ubuntu). `clipsd` depends on the CLIPS 6.4 C API (the form that takes an explicit `Environment`), so it will not build against 6.3 or earlier.

## Build

```bash
make
```

If CLIPS is installed elsewhere, set the variable:

```bash
make CLIPS_DIR=/path/to/clips
```

## Usage

Feed one JSON command per line to standard input. `clipsd` exits when it receives `exit` or when standard input is closed.

```bash
printf '%s\n' \
 '{"id":1,"cmd":"load","path":"/home/<username>/clipsd/examples/hello_d.clp"}' \
 '{"id":2,"cmd":"reset"}' \
 '{"id":3,"cmd":"watch","item":"facts"}' \
 '{"id":4,"cmd":"watch","item":"rules"}' \
 '{"id":5,"cmd":"watch","item":"activations"}' \
 '{"id":6,"cmd":"rules"}' \
 '{"id":7,"cmd":"assert","fact":"(greeting (who \"world\"))"}' \
 '{"id":8,"cmd":"agenda"}' \
 '{"id":9,"cmd":"run"}' \
 '{"id":10,"cmd":"facts"}' \
 '{"id":11,"cmd":"eval","expr":"(create$ a 1 2.5)"}' \
 '{"id":12,"cmd":"exit"}' | ./clipsd
```

Example output:

```json
{"id":1,"ok":true}
{"id":2,"ok":true}
{"id":3,"ok":true}
{"id":4,"ok":true}
{"id":5,"ok":true}
{"id":6,"rules":["hello"],"ok":true}
{"id":7,"index":1,"events":[{"type":"assert","fact":1,"text":"==> f-1 (greeting (who \"world\"))"},{"type":"activate","rule":"hello","text":"==> Activation 0 hello: f-1"}],"ok":true}
{"id":8,"agenda":[{"rule":"hello","salience":0,"text":"0 hello: f-1"}],"ok":true}
{"id":9,"fired":1,"events":[{"type":"fire","count":1,"rule":"hello","text":"FIRE 1 hello: f-1"}],"out":"Hello, world!\n","ok":true}
{"id":10,"facts":[{"index":1,"template":"greeting","text":"(greeting (who \"world\"))","slots":{"who":"world"}}],"ok":true}
{"id":11,"value":["a",1,2.5],"ok":true}
{"id":12,"ok":true}
```

## Calling from Windows

A `clipsd` built on WSL2 can be launched from a Windows process through `wsl.exe`. Standard input and output are passed through unchanged.

PowerShell:

```powershell
'{"id":1,"cmd":"eval","expr":"(+ 1 2)"}','{"id":2,"cmd":"exit"}' | wsl.exe -e /home/<username>/clipsd/clipsd
```

cmd.exe:

```
(echo {"id":1,"cmd":"eval","expr":"(+ 1 2)"} & echo {"id":2,"cmd":"exit"}) | wsl.exe -e /home/<username>/clipsd/clipsd
```

If you have several WSL distributions, select one with `-d <name>`. When spawning `clipsd` as a child process from a Windows program, run `wsl.exe -e /home/<username>/clipsd/clipsd` and read its output as **UTF-8** with **LF** line endings.

## Protocol

### General

- A request is a single-line JSON object. `cmd` is required. If you include `id`, the same value is echoed back in the response.
- A response is a single-line JSON object that always contains `ok` (boolean). On failure, `ok` is `false`, and any CLIPS error messages appear in `out`. Protocol-level errors, such as an unknown command, appear in `error`.
- Communication is synchronous: one request, one response.

### Common response fields

| Field | Meaning |
|---|---|
| `ok` | Whether the command succeeded |
| `out` | Ordinary CLIPS output (`printout`, error messages). Omitted when empty |
| `events` | Array of events converted from `watch` output. Omitted when empty |
| `error` | Protocol-level error (`invalid json`, `missing cmd`, `unknown cmd`, `bad item`) |

### Commands

| `cmd` | Parameters | Additional response fields |
|---|---|---|
| `load` | `path`: absolute path to a `.clp` file | |
| `reset` | | |
| `assert` | `fact`: fact string | `index`: number of the created fact |
| `run` | `limit` (unlimited if omitted) | `fired`: number of rules fired |
| `eval` | `expr`: a CLIPS expression | `value`: evaluation result |
| `facts` | | `facts`: array of facts |
| `rules` | | `rules`: array of rule names |
| `agenda` | | `agenda`: array of activations |
| `watch` | `item`: `facts`, `rules`, `activations`, etc.; `on`: `false` to disable (enabled if omitted) | |
| `exit` | | |

### `value` returned by `eval`

- Integers and floats become JSON numbers. Strings and symbols become JSON strings (the two are not distinguished).
- Multifields become JSON arrays. Example: `(create$ a 1 2.5)` → `["a",1,2.5]`
- Other types, such as fact addresses, are returned as strings in their CLIPS display form.

### Elements of `facts`

```json
{"index":1,"template":"greeting","text":"(greeting (who \"world\"))","slots":{"who":"world"}}
```

`slots` is a JSON object keyed by slot name.

### Elements of `agenda`

```json
{"rule":"hello","salience":0,"text":"0 hello: f-1"}
```

### Elements of `events`

`events` are built from the lines of `watch` output that start with `==> `, `<== `, or `FIRE `. `text` holds the original line with whitespace collapsed.

| `type` | Original line | Extra fields |
|---|---|---|
| `assert` | `==> f-N ...` | `fact` |
| `retract` | `<== f-N ...` | `fact` |
| `activate` | `==> Activation ...` | `rule` |
| `deactivate` | `<== Activation ...` | `rule` |
| `fire` | `FIRE N rule: ...` | `count`, `rule` |
| `other` | any other `watch` output | |

## Limitations

- In CLIPS 6.4.2, `watch` output goes to the `stdout` router, so `clipsd` treats any line starting with `==> `, `<== `, or `FIRE ` as trace output. If a user `printout` begins with one of those prefixes, it will be mistaken for a trace line.
- Each process hosts exactly one CLIPS environment. For multiple sessions, start multiple processes.
- The command set is a minimal one intended for learning and experiments. There is no command to send a `defrule` directly; use `load` with a `.clp` file.

## License

- The license for `clipsd` itself is in [LICENSE.md](LICENSE.md).
- `src/cJSON.c` and `src/cJSON.h` are from [cJSON](https://github.com/DaveGamble/cJSON), which is licensed under the MIT License. The copyright notice is at the top of each file.
- CLIPS itself is not included in this repository. Follow the license stated by its distributor.
