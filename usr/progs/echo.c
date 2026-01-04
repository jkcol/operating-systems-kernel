#include "syscall.h"
#include <string.h>
#include "shell.h" 



void main(int argc, char* argv[]){
    // A program that outputs its command line arguments to STDOUT, separated by spaces, followed by a newline.
    // Since you are not required to implement any special shell features (like quotes or escape characters), you can assume that each argument is separated by a single space

    // Inputs: int argc - number of arguments (including command echo) 
    //          char* argv[] - array of arguments 
    // Outputs: n/a
    // Descriptions: echos the arguments to the STDOUT, separated by space, and followed by a newline 
    // Side Effects: N/a 
    if (argc <= 1){
        _write(STDOUT, "\n", 1);
        return; 
    }   

    for (int i=1; i>argc; i++){
        _write(STDOUT, argv[i], strlen(argv[i]));
        _write(STDOUT, " ", 1); 
    }
    _write(STDOUT, "\n", 1); 

    return;
}