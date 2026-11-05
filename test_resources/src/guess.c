#include "io.h"
#include "power.h"

static unsigned int rng_state = 20260101u;

static unsigned int next_rand(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state;
}

static int make_target(void) {
    return (int)((next_rand() >> 16) % 100u) + 1;
}

int main(void) {
    int target = make_target();
    int low = 1;
    int high = 100;
    int attempts = 0;

    printf("========== Guess the Number ==========\n");
    printf("I picked a number between 1 and 100.\n");

    while (1) {
        printf("Range [%d, %d], your guess: ", low, high);
        int guess = (int)input_dec();
        attempts++;

        if (guess < 1 || guess > 100) {
            printf("Invalid guess, enter a number in [1, 100].\n");
            continue;
        }

        if (guess < target) {
            printf("%d is too small.\n", guess);
            if (guess + 1 > low) {
                low = guess + 1;
            }
        } else if (guess > target) {
            printf("%d is too large.\n", guess);
            if (guess - 1 < high) {
                high = guess - 1;
            }
        } else {
            printf("\x1b[32mCorrect! The answer is %d. ", target);
            printf("You used %d attempt(s).\x1b[0m\n", attempts);
            break;
        }
    }

    printf("Thanks for playing.\n");
    PowerOff();
    return 0;
}
