#include "app_intrf.h"
#include "util_funcs/log_funcs.h"
#include "types.h"
#include <stdlib.h>
#include <termios.h>
#include <stdio.h>
#include <unistd.h>

struct termios orig_termios;

static void disableRawMode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}
static void enableRawMode() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disableRawMode);
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(){
    enableRawMode();
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
    //if app_intrf failed to initialize analyze the error write it and exit
    if(!app_intrf){
	log_append_logfile("Could not start the app_intrf\n");
        exit(1);
    }

    while(1){
	//erase the terminal
	printf("\033[2J\033[H");
	//update the interface, of course should be in a loop
	nav_update(app_intrf);
    
	//Get the context interface
	//----------------------------------------------------------------------------------------------------
	CX* cx_curr = nav_cx_curr_return(app_intrf);
	if(cx_curr){
	    char display_name[MAX_PARAM_NAME_LENGTH];
	    if(nav_cx_display_name_return(app_intrf, cx_curr, display_name, MAX_PARAM_NAME_LENGTH) == 1){
		printf("---> %s\n", display_name);
	    }
	    
	    unsigned int count = 0;
	    CX** cx_children = nav_cx_children_return(app_intrf, cx_curr, &count);
	    for(unsigned int i = 0; i < count; i++){
		CX* cx_child = cx_children[i];
		//highlight the selected context
		CX* selected_cx = nav_cx_selected_return(app_intrf);
		if(selected_cx){
		    if(selected_cx == cx_child){
			printf("\033[0;30;47m");
		    }
		}
		printf("     |");

		//show the name of the context
		if(nav_cx_display_name_return(app_intrf, cx_child, display_name, MAX_PARAM_NAME_LENGTH) == 1){
		    printf("--> %s", display_name);
		}	    
		//reset highlighting
		printf("\n\033[0m");
	    }

	    //print the top array
	    count = 0;
	    CX** top_cx_array = nav_cx_top_children_return(app_intrf, &count);
	    printf("--------------------------------------------------\n");
	    for(unsigned int i = 0; i < count; i++){
		CX* top_cx = top_cx_array[i];
		if(nav_cx_display_name_return(app_intrf, top_cx, display_name, MAX_PARAM_NAME_LENGTH) == 1){
		    printf("%s | ", display_name);
		}
	    }
	    printf("\n--------------------------------------------------\n");
	}
	//----------------------------------------------------------------------------------------------------

	//get user inputs
	char input = getchar();
	unsigned int exit = 0;
	switch(input){
	case 'd':
	    nav_cx_selected_next(app_intrf);
	    break;
	case 'a':
	    nav_cx_selected_prev(app_intrf);
	    break;
	case 'e':
	    if(nav_cx_selected_invoke(app_intrf) == -1)
		exit = 1;
	    break;
	case 'q':
	    if(nav_cx_curr_exit(app_intrf) == -1)
	       exit = 1;
	    break;
	}
	
	if(exit == 1)
	    break;
    }
    
    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    disableRawMode();
    return 0;
}
