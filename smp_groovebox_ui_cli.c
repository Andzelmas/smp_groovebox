#include "app_intrf.h"
#include "util_funcs/log_funcs.h"
#include "types.h"
#include <stdlib.h>
#include <stdio.h>

int main(){
    log_clear_logfile();
    APP_INTRF *app_intrf = app_intrf_init();
    //if app_intrf failed to initialize analyze the error write it and exit
    if(!app_intrf){
	log_append_logfile("Could not start the app_intrf\n");
        exit(1);
    }
    //TODO first get user input
    
    //update the interface, of course should be in a loop
    nav_update(app_intrf);
    
    //NAVIGATING interface
    CX* cx_curr = nav_cx_curr_return(app_intrf);
    if(cx_curr){
	char display_name[MAX_PARAM_NAME_LENGTH];
	if(nav_cx_display_name_return(app_intrf, cx_curr, display_name, MAX_PARAM_NAME_LENGTH) == 1){
	    printf("---> %s\n", display_name);
	}

	CX* cx_child = nav_cx_child_return(app_intrf, cx_curr, 0);
	unsigned int child_idx = 0;
	while(cx_child){
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
	    child_idx += 1;
	    cx_child = nav_cx_child_return(app_intrf, cx_curr, child_idx);
	    
	    //reset highlighting
	    printf("\n\033[0m");
	}

	//print the top array
	CX* top_cx = nav_cx_top_child_return(app_intrf, 0);
	child_idx = 0;
	printf("--------------------------------------------------\n");
	while(top_cx){
	    if(nav_cx_display_name_return(app_intrf, top_cx, display_name, MAX_PARAM_NAME_LENGTH) == 1){
		printf("%s | ", display_name);
	    }
	    child_idx += 1;
	    top_cx = nav_cx_top_child_return(app_intrf, child_idx);
	}
	printf("\n--------------------------------------------------\n");
    }
    //----------------------------------------------------------------------------------------------------
    
    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    return 0;
}
