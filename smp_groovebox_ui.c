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
    //update the interface, of course should be in a loop
    nav_update(app_intrf);
    
    //NAVIGATING interface
    CX* cx_curr = nav_cx_curr_return(app_intrf);
    if(cx_curr){
	char display_name[MAX_PARAM_NAME_LENGTH];
	if(nav_cx_display_name_return(app_intrf, cx_curr, display_name, MAX_PARAM_NAME_LENGTH) == 1){
	    printf("cx_curr: %s\n", display_name);
	}

	CX* cx_child = nav_cx_child_return(app_intrf, cx_curr, 0);
	unsigned int child_idx = 0;
	while(cx_child){
	    printf("   |");
	    if(nav_cx_display_name_return(app_intrf, cx_child, display_name, MAX_PARAM_NAME_LENGTH) == 1){
		printf("--> %s\n", display_name);
	    }
	    child_idx += 1;
	    cx_child = nav_cx_child_return(app_intrf, cx_curr, child_idx);
	}
    }
    //----------------------------------------------------------------------------------------------------
    
    app_intrf_destroy(app_intrf);

    log_append_logfile("Cleaned everything, closing the app \n");
    return 0;
}

