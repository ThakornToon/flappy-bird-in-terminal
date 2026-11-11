# Flappy Bird CLI

A small Flappy Bird-style game written in C. It runs in a terminal and uses
ASCII characters to draw the playfield, bird, and obstacles.

## Requirements

- A C compiler
- `make`
- A terminal that supports ANSI escape sequences

The game must be run in an interactive terminal; redirected input or output is
not supported.

## Build

Build both executable names with:

```sh
make
```

Run either executable:

```sh
./flappy-bird
# or
./main
```

To remove the generated executables:

```sh
make clean
```

## How to play

- Press **Space** on the start screen to begin.
- Press **Space** during a round to flap upward. Gravity pulls the bird down.
- Move left or right with **A/D** or the **Left/Right Arrow** keys.
- Avoid colliding with obstacles. Vertical pipes move from right to left, and
  horizontal pipes descend from above. Find and pass through their gaps.
- Press **R** or **Space** after Game Over to start a new round.
- Press **Q** at any time to quit.

The playfield is 72 columns by 28 rows and targets approximately 30 frames per
second.

## Scoring

- Successfully passing an obstacle awards one point.
- A collision subtracts two points. The game ends when the score drops below
  zero.
- The Game Over screen shows the highest score reached during the current
  round. This maximum score resets to zero when a new round starts.

The game restores the terminal settings and cursor when it exits.
