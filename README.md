# meowsh

A POSIX-compliant shell implementation.

## Features

- POSIX compliance (WIP)
- Built-in commands (alias, jobs, etc.)
- Line editing support
- Job control
- Variable expansion
- Redirections
- Auto-detect Starship in interactive mode and render prompt via `starship prompt`

## Building

To build `meowsh`, run:

```bash
make
```

## Usage

After building, you can start the shell with:

```bash
./meowsh
```

To disable automatic Starship initialization, set `MEOWSH_STARSHIP=0` before starting `meowsh`.

## Development

The source code is written in Go and located in the `src/` directory.
