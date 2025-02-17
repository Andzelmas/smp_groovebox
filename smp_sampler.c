#include <stdio.h>
#include <stdlib.h>

//my libraries includes
//functions to interact with the app_data, user interface basically
#include "app_intrf.h"
#include "util_funcs/log_funcs.h"
//maximum length for the display_text of the window
#define MAX_DISPLAY_TEXT 100

int main(){
    //clear the log file
    log_clear_logfile();
    //init the app interface
    intrf_status_t intrf_status = 0;
    APP_INTRF *app_intrf = NULL;
    app_intrf = app_intrf_init(&intrf_status, NULL);
    //if app_intrf failed to initialize analyze the error write it and exit
    if(app_intrf==NULL){
        const char* err = app_intrf_write_err(&intrf_status);
        fprintf(stderr,"%s\n", err);
        exit(1);
    }
    //if error occured but the app_intrf initialized, parse the error and continue
    if(intrf_status<0){
        const char* err = app_intrf_write_err(&intrf_status);
        fprintf(stderr,"%s\n", err);
    }

    while(1){
        char q;
        scanf("%c",&q);
	//get the selected cx
	CX* sel_cx = NULL;
	sel_cx = nav_ret_select_cx(app_intrf);

	//enter the selected cx, also showing its value if thats a sort of cx that has a value
	char* disp_string = malloc(sizeof(char) * MAX_DISPLAY_TEXT);
	int disp_err = nav_get_cx_value_as_string(app_intrf, sel_cx, disp_string, MAX_DISPLAY_TEXT);
	if(disp_string){
	    if(disp_err == 0)printf("Val %s\n", disp_string);
	    free(disp_string);
	}

        if(q=='q'){
	    int exit_err =  nav_exit_cur_context(app_intrf);
            if(exit_err==-2){
                break;
            }
	    sel_cx = nav_ret_select_cx(app_intrf);
	    printf("In %s\n", nav_get_cx_name(app_intrf, sel_cx));
        }
        if(q=='e'){
	    printf("->%s\n", nav_get_cx_name(app_intrf, sel_cx));
	    //invoke can destroy cx that is why we get a new selected cx from that
	    int changed = 0;
	    CX* new_select = nav_invoke_cx(app_intrf, sel_cx, &changed);
	    if(new_select)sel_cx = new_select;
	    nav_enter_cx(app_intrf, sel_cx);		
        }
        if(q=='d'){
            nav_next_context(app_intrf);
	    sel_cx = nav_ret_select_cx(app_intrf);
	    printf("%s\n", nav_get_cx_name(app_intrf, sel_cx));
        }
        if(q=='a'){
            nav_prev_context(app_intrf);
	    sel_cx = nav_ret_select_cx(app_intrf);
	    printf("%s\n", nav_get_cx_name(app_intrf, sel_cx));	    
        }
	if(q=='r'){
	    //set the value of the sel_cx to increase
	    nav_set_cx_value(app_intrf, sel_cx, 1);
	}
	if(q=='f'){
	    //set the value of the sel_cx to decrease
	    nav_set_cx_value(app_intrf, sel_cx, -1);
	}
	nav_update_params(app_intrf);
    }
   
    return 0;
}


