#include "types.h"
#include "stat.h"
#include "user.h"
#include "syscall.h"

int main(int argc, char *argv[]) {
  int i;
  int buffSize = 1024;
  char *pages[80];  
  printf (1, "\n\n\n#########################\n\tTests:\n#########################\n\n\n");

  printf (1, "\tAnalyze after init...\n\n\n");
  sleep(500);

  // TEST1
  printf (1, "\n\nTest1: Allocation\n");
  for(i = 0; i<80; i++) {
    pages[i] = malloc(buffSize);
    if (i == 5) {
      printf (1, "\tAnalyze after 5th allocation...\n\n\n");
      sleep(500);
    }
  }
  printf (1, "\tAnalyze end of allocation...\n\n\n");
  sleep(500);

  // TEST2
  printf (1, "\n\nTest2: Writing\n");
  for(i = 0; i<80; i++) {
    pages[i][0] = 10;
  }
  printf (1, "\tAnalyze after writing...\n\n\n");
  sleep(500);

  // TEST3
  printf (1, "\n\nTest3: Reading\n");
  for(i = 0; i<80; i++)
    printf(1, "\t\tReading values... (%d)\n", pages[i][0]);
  printf (1, "\n\tAnalyze after reading...\n\n\n");
  sleep(500);

  for(i = 0; i<80; i++) {
    free(pages[i]);
  }

  // TEST4
  printf (1, "\n\nTest4: Fork\n");
  for(i = 0; i<80; i++) {
    pages[i] = malloc(buffSize);
    pages[i][0] = 10;
  }
  int pid = fork();
  if(pid == 0) {
    
    for(i = 0; i<80; i++) 
      pages[i][0] = 10;
    printf (1, "\tAnalyze after child writing...\n\n\n");
    sleep(500);

    for(i = 0; i<80; i++) 
      printf(1, "\t\tReading values... (%d)\n", pages[i][0]);
    printf (1, "\n\tAnalyze after child reading...\n\n\n");
    sleep(500);
    
    exit();
  }
  wait();
  printf (1, "\n\tAnalyze after child exit...\n\n\n");
  
  for(i = 0; i<80; i++) {
    free(pages[i]);
  }

  exit();
}