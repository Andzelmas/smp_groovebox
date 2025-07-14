#include "app_intrf.h"
#include "util_funcs/log_funcs.h"
#include "types.h"
#include <stdlib.h>
#include <stdio.h>

int main(){
    //erase the terminal
    printf("\033[2J\033[H");
    
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
    //if app_intrf failed to initialize analyze the error write it and exit
    if(!app_intrf){
	log_append_logfile("Could not start the app_intrf\n");
        exit(1);
    }

    while(1){
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
	char input;
	scanf("%c", &input);
	if(input == 'd'){
	    nav_cx_selected_next(app_intrf);
	}
	if(input == 'a'){
	    nav_cx_selected_prev(app_intrf);
	}
	if(input == 'e'){
	    if(nav_cx_selected_invoke(app_intrf) == -1)
		break;
	}
	if(input == 'q'){
	    if(nav_cx_curr_exit(app_intrf) == -1)
		break;
	}
	//erase the terminal
	printf("\033[2J\033[H");
    }
    
    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    return 0;
}
