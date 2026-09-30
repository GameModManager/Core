/* Guard for the LSan suppression list.
 *
 * Deliberately leaks memory allocated in our own code (the stack is plain
 * malloc <- main, which matches no suppression entry). LSan must still
 * report it: ctest registers this binary with
 * PASS_REGULAR_EXPRESSION "detected memory leaks", so if the suppression
 * list ever grows broad enough to hide our own leaks, this test turns red.
 *
 * Only registered when a leak sanitizer is compiled in - see the guard in
 * tests/CMakeLists.txt. A default build has no -fsanitize flag, so the leak
 * below is invisible and there is nothing for the regex to match.
 */
#include <stdlib.h>

int main(void) {
  volatile void *leak = malloc(101);
  (void)leak;
  return 0;
}
