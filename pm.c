#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

#ifdef __APPLE__
#include <termios.h>
#endif
#ifdef __linux__
#include <termio.h>
#endif

#define FR 2

// Low level terminal handling.
// Taken from https://github.com/antirez/kilo

static struct termios orig_termios; /* In order to restore at exit.*/

void disable_raw_mode(void) {
  // Don't even check the return value as it's too late.
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

int enable_raw_mode(int fd) {
  struct termios raw;

  if (!isatty(STDIN_FILENO))
    goto fatal;
  atexit(disable_raw_mode);
  if (tcgetattr(fd, &orig_termios) == -1)
    goto fatal;

  raw = orig_termios; /* modify the original mode */
  /* input modes: no break, no CR to NL, no parity check, no strip char,
   * no start/stop output control. */
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  /* output modes - disable post processing */
  raw.c_oflag &= ~(OPOST);
  /* control modes - set 8 bit chars */
  raw.c_cflag |= (CS8);
  /* local modes - choing off, canonical off, no extended functions,
   * no signal chars (^Z,^C) */
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  /* control chars - set return condition: min number of bytes and timer. */
  raw.c_cc[VMIN] = 0;  /* Return each byte, or zero for timeout. */
  raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

  /* put terminal in raw mode after flushing */
  if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
    goto fatal;
  return 0;

fatal:
  errno = ENOTTY;
  return -1;
}

enum key {
  KEY_NULL = 0,    /* NULL */
  CTRL_C = 3,      /* ctrl-c */
  CTRL_D = 4,      /* ctrl-d */
  CTRL_F = 6,      /* ctrl-f */
  CTRL_H = 8,      /* ctrl-h */
  TAB = 9,         /* tab */
  CTRL_L = 12,     /* ctrl+l */
  ENTER = 13,      /* enter */
  CTRL_Q = 17,     /* ctrl-q */
  CTRL_S = 19,     /* ctrl-s */
  CTRL_U = 21,     /* ctrl-u */
  ESC = 27,        /* escape */
  BACKSPACE = 127, /* backspace */
  /* The following are just soft codes, not really reported by the
   * terminal directly. */
  LEFT = 1000,
  RIGHT,
  UP,
  DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

int readkey(int fd) {
  int nread;
  char c, seq[3];
  while ((nread = read(fd, &c, 1)) == 0)
    ;
  if (nread < 0)
    exit(1);

  while (1) {
    switch (c) {
    case ESC: /* escape sequence */
      /* If this is just an ESC, we'll timeout here. */
      if (read(fd, seq, 1) == 0)
        return ESC;
      if (read(fd, seq + 1, 1) == 0)
        return ESC;

      /* ESC [ sequences. */
      if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
          /* Extended escape, read additional byte. */
          if (read(fd, seq + 2, 1) == 0)
            return ESC;
          if (seq[2] == '~') {
            switch (seq[1]) {
            case '3':
              return DEL_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            }
          }
        } else {
          switch (seq[1]) {
          case 'A':
            return UP;
          case 'B':
            return DOWN;
          case 'C':
            return RIGHT;
          case 'D':
            return LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
          }
        }
      }

      /* ESC O sequences. */
      else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
      break;
    default:
      return c;
    }
  }
}

#define INF INT32_MAX

#define GAME_WIN -10
#define GAME_LOSE -11

#define GHOST_HUNGER 1 // set when ghost eats food

#define DIR_LEFT 1
#define DIR_RIGHT 2
#define DIR_UP 4
#define DIR_DOWN 8

#define SPACE ' '
#define WALL '#'
#define PLAYER '.'


#define append(p, b, n)                                                        \
  {                                                                            \
    memcpy(p->render + p->len, b, n);                                          \
    p->len += n;                                                               \
  }

#define isfood(c) (c >= '0' && c <= '9')
#define isghost(c) (c >= 'A' && c <= 'Z')

#define at(p, x, y) ((p)->buf[(y) * (p)->w + (x)])
#define player(p) at(p, p->x, p->y)

#define dis(x1, y1, x2, y2) (abs(x1-x2)+abs(y1-y2)) // Manhanttan Distance

struct food {
  char c;
  const char *p;
  uint8_t size;
  uint8_t score;
};

struct ghost {
  char c;
  const char *p;
  uint8_t size;
  uint8_t speed;
  uint8_t flag;
  // field below only used at runtime
  int x, y;
  char bypass;
};

struct pacman {
  char *buf; // internal array used to generate pacman::render.
  int w, h; // shape of the map.
  int x, y; // position of the player.
  int out;  // file descriptor to render the entire game to.
  int score; // scores the player gets.
  int goal;  // scores the player have to earn to win the game.

  char *render; // like GPU memory used to render the entire game to pacman::out.
  int len; // length of pacman::render

  struct ghost ghosts[12];
  int ghosts_len;
};

static struct food foods[] = {
    {'1', "💩", 4, 1},
    {'2', "🍰", 4, 5},
    {'3', "🥩", 4, 10},
    {'4', "🍭", 4, 50},
};

static struct ghost ghosts[] = {
    {'A', "😈", 4, 1, 0},
    {'B', "👻", 4, 1, 0},
    {'C', "👹", 4, 4, GHOST_HUNGER},
};

// TODO: fix the bug in function ghost_move
int ghost_move(struct pacman *p, int i) {
  struct ghost *g = p->ghosts+i;
  int px = p->x, py = p->y; // current player position
  int gx = g->x, gy = g->y; // current ghost position
  int d, move, cost = INF;
  char n, l, r, t, b;

  // we don't always have to move the ghosts
  // to lower the difficulty of the game.
  if (rand() % 2 == 0)
    return 0;

  // see if it's shorter one step to the left.
  l = at(p, gx-1, gy);
  if (l != WALL) {
    d = dis(gx-1, gy, px, py);
    if (d < cost) {
      cost = d;
      move = DIR_LEFT;
    }
  }
  // see if it's shorter one step to the right.
  r = at(p, gx+1, gy);
  if (r != WALL) {
    d = dis(gx+1, gy, px, py);
    if (d < cost) {
      cost = d;
      move = DIR_RIGHT;
    }
  }
  // see if it's shorter one step to the top.
  t = at(p, gx, gy-1);
  if (t != WALL) {
    d = dis(gx, gy-1, px, py);
    if (d < cost) {
      cost = d;
      move = DIR_UP;
    }
  }
  // see if it's shorter one step to the bottom.
  b = at(p, gx, gy+1);
  if (b != WALL) {
    d = dis(gx, gy+1, px, py);
    if (d < cost) {
      cost = d;
      move = DIR_DOWN;
    }
  }

  // update the ghost coordinate with the direction we just chose.
  switch (move) {
  case DIR_LEFT:
    g->x--;
    n = l;
    break;
  case DIR_RIGHT:
    g->x++;
    n = r;
    break;
  case DIR_UP:
    g->y--;
    n = t;
    break;
  case DIR_DOWN:
    g->y++;
    n = b;
    break;
  }

  // if the player is caught by this ghost, game over.
  if (n == PLAYER)
    return GAME_LOSE;

  // if the chosen position to move to stands also a ghost 
  // and it's superior than the current one, we stand still.
  if (isghost(n))
    return 0;

  at(p, gx, gy) = g->bypass;
  g->bypass = n;
  at(p, g->x, g->y) = g->c;
  return 0;
}


void pacman_init(struct pacman *p, const char *path) {
  FILE *fp;
  char *lp = NULL, c;
  size_t n;
  ssize_t nr;
  long size;
  int w = 0, h = 0;
  int i;
  struct ghost *ghost;
  struct food *food;

  if ((fp = fopen(path, "r")) == NULL) {
    perror("fopen");
    exit(1);
  }

  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  p->buf = (char *)malloc(size);
  p->x = p->y = 0;
  p->ghosts_len = 0;
  p->score = 0;
  p->goal = 0;
  p->out = STDOUT_FILENO;
  p->len = 0;

  while ((nr = getline(&lp, &n, fp)) > 0) {
    if (w != 0 && w != nr - 1) {
      fprintf(stdout, "bad map shape");
      exit(1);
    }
    w = nr - 1;
    memcpy(p->buf + (h * w), lp, w);
    for (i = 0; i < nr; i++) {
      c = lp[i];
      if (isghost(c)) {
        ghost = ghosts + (c - 'A');
        ghost->x = i;
        ghost->y = h;
        ghost->bypass = SPACE;
        p->ghosts[p->ghosts_len] = *ghost; // copy
        p->ghosts_len++;
      } else if (c == PLAYER) {
        p->x = i;
        p->y = h;
      } else if (isfood(c)) {
        food = foods + (c - '0');
        p->goal += food->score;
      }
    }

    h++;
  }

  p->w = w;
  p->h = h;
  p->render = (char *)malloc(w * h * 4 * 4 + 64);

  free(lp);
  fclose(fp);
}

void pacman_free(struct pacman *p) {
  free(p->buf);
  free(p->render);
}

static int render(struct pacman *p) {
  int i, j, n;
  char buf[64], c;
  struct food *food;
  struct ghost *ghost;

  p->len = 0;
  append(p, "\x1b[?251\x1b[H", 9);
  n = sprintf(buf, "Pac-Man v0.1   Scores: %.4d  Player: %.2d,%.2d\r\n",
              p->score, p->x, p->y);
  append(p, buf, n);

  for (i = 0; i < p->h; i++) {
    for (j = 0; j < p->w; j++) {
      c = at(p, j, i);
      if (isfood(c)) {
        food = foods + (c - '0');
        append(p, food->p, food->size);
      } else if (isghost(c)) {
        ghost = ghosts + (c - 'A');
        append(p, ghost->p, ghost->size);
      } else {
        switch (c) {
        case WALL:
          append(p, "🟦", 4);
          break;
        case SPACE:
          append(p, "  ", 2);
          break;
        case PLAYER:
          append(p, "😋", 4);
          break;
        }
      }
    }
    append(p, "\r\n", 2);
  }
  return write(p->out, p->render, p->len);
}

int next(struct pacman *p, int key) {
  int w = p->w, h = p->h;
  int x = p->x, y = p->y;
  int nx = x, ny = y;
  int i, retval;
  char c;
  if (key != KEY_NULL) {
    switch (key) {
    case LEFT:
      nx = (x - 1) % w;
      if (at(p, nx, y) != WALL)
        p->x = nx;
      break;
    case RIGHT:
      nx = (x + 1) % w;
      if (at(p, nx, y) != WALL)
        p->x = nx;
      break;
    case UP:
      ny = (y - 1) % h;
      if (at(p, x, ny) != WALL)
        p->y = ny;
      break;
    case DOWN:
      ny = (y + 1) % h;
      if (at(p, x, ny) != WALL)
        p->y = ny;
      break;
    }
    at(p, x, y) = SPACE;
    c = player(p);
    if (isfood(c))
      p->score += foods[c - '0'].score;
    if (p->score == p->goal)
      return GAME_WIN;
    player(p) = PLAYER;
  }

  // move ghosts close to the player.
  for (i = 0; i < p->ghosts_len; i++) {
    if ((retval = ghost_move(p, i)) < 0)
      return retval;
  }
  return render(p);
}

int main(int argc, char **argv) {
  struct timeval tv;
  fd_set rfds, _rfds;
  int retval, key;
  struct pacman p;

  if (argc != 2) {
    fprintf(stdout, "Usage: %s <filename>\n", argv[0]);
    exit(1);
  }

  srand(time(NULL));

  pacman_init(&p, argv[1]);

  FD_ZERO(&rfds);
  FD_SET(STDIN_FILENO, &rfds);

  enable_raw_mode(STDIN_FILENO);
  system("clear");

  while (1) {
    _rfds = rfds;
    tv.tv_sec = 0;
    tv.tv_usec = 1000 / FR * 1000;
    retval = select(1, &_rfds, NULL, NULL, &tv);
    if (retval < 0) {
      perror("select");
      exit(1);
    }
    if (retval > 0) {
      key = readkey(STDIN_FILENO);
      switch (key) {
      case CTRL_C:
        goto done; // we exit early here
      }
    } else {
      key = KEY_NULL;
    }
    // handle next frame
    retval = next(&p, key);
    if (retval >= 0)
      continue;
    
    switch (retval) {
    case GAME_LOSE:
      printf("You lose!\r\n"); // we are still in raw mode.
      goto done;
    case GAME_WIN:
      printf("You win!\r\n");
      goto done;
    default:
      perror("next");
      exit(1);
    }
  }
done:
  pacman_free(&p);
  return 0;
}
