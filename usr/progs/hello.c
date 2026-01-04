
#include "string.h"
#include "syscall.h"
void main(void) {
    printf("I am the parent! \n");
    int ret = _fork();
    if(ret == 0){
        printf("Hello, world! (child) \n");
        return;
    }
    else{
        _wait(ret);
        printf("Hello, world (parent)!\n");
    }
}