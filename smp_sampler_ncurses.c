#include <stdio.h>
#include <stdlib.h>
#include <ncurses.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
//my libraries includes
//functions to interact with the app_data, user interface basically
#include "app_intrf.h"
//functions for log file
#include "util_funcs/log_funcs.h"

//max tick before it restarts
#define MAX_TICK 10000
//max windows that can fit in a window, after that scroll
//if the screen is even smaller and less windows fit, the scroll bar will appear sooner
#define MAX_WINDOWS 8

//window struct that holds the text to display in the window, the ncurses window object and cx struct if
//aplicable
typedef struct _win_impl_ WIN;
typedef struct _win_impl_{
    char* display_text;
    //from what character to display the text, if it does not fit into the window
    unsigned int text_start;
    //the text to display, this can be the display_text, but if it does not fit, it will be scroll text
    char* scroll_text;
    //does the window depend on the tick (should we refresh/clear it each tick
    unsigned int anim;
    //what type of window this is, for example a parameter | name of the parameter or parameter | value
    //of the param. Depending on this we know what the user clicked and can for ex. increase the value of
    //the param. Also win_type can be scroll up or down button, context exit button, etc.
    unsigned int win_type;
    //if this is a container window, that contains other windows (for example a window that contains parameters)
    //this array will be filled with those windows
    WIN** children_array;
    //the size of the children array
    unsigned int children_array_size;
    int height, width;
    int win_y, win_x;
    //highlight==1 change box frame, highlight==2 highlight with color.
    int highlight;
    //if hide==1 we dont refresh the window, hide == 2 dont layout this window
    int hide;
    WINDOW* nc_win;
    CX* cx_obj;
}WIN;

//enum for win types
enum win_type_enum{
    //the window is the title window in the upper left corner
    Title_win_type  = 0x0100,
    //scrollbar window, to scroll through windows that do not fit in the main area
    Scroll_win_type = 0x0200,
    Scroll_up_win_type = 0x0001,
    Scroll_down_win_type = 0x0002,
    //parameter window, with subtypes name (to decrease param value) and value (to increase param value)
    Param_win_type = 0x0300,
    Param_name_win_type = 0x0001,
    Param_val_win_type = 0x0002
};

//the screen object, that holds all the windows and window arrays for the current screen
typedef struct _curr_screen_impl_{
    //a tick for controlling animations, goes up to 1000 then restarts
    unsigned int tick;
    //which line of the logfile is currently read
    unsigned int log_line;
    //the win array displayed in the middle window that holds the cx in the middle window
    WIN** win_array;
    //how many windows from cx are created in the middle window array (win_array)
    unsigned int win_array_size;
    //from which member of the win_array we should display the windows
    unsigned int win_array_start;
    //the last member of the win_array that is displayed because of scrolling
    int win_array_end;
    //the win array displayed in the button window
    WIN** button_win_array;
    unsigned int button_win_array_size;
    //the win that we see all the time at the bottom for fast navigation;
    WIN** root_win_array;
    //how many windows for the bottom navigation are created
    unsigned int root_win_array_size;    
    //the window for the title of the curr_cx on the left side and small info on the right side
    WIN* title_win;
    //the left and right title windows, left holds curr_cx, the right useful info like log messages
    WIN** title_lr_win;
    //the main window, where the main cx are placed
    WIN* main_win;
    //the two windows inside the title_lr_win[1]. Left one holds the info window,
    //right - the main_lt_scroll_win
    WIN** title_scroll_win;
    //the scroll windows array that is in the title_scroll_win[1], it holds the scroll up and scroll down buttons
    WIN** scroll_bar_wins;
    //window that has context manipulation buttons, like save song or load preset or add sample
    WIN* button_win;    
    //bottom window where the root_cx children are placed for fast navigation - these can be accessed anywhere
    WIN* fast_win;
    //The currently selected window, holds one of main_win, button_win or fast_win
    WIN* curr_main_win;
    //currently selected cx window in the curr_main_win, like save button in sampler cx window or similar
    //TODO navigation between these is with wsad keys - on a grid of windows
    WIN* curr_cx_win;
}CURR_SCREEN;

//initialize the main screen struct
int curr_screen_init(APP_INTRF* app_intrf, CURR_SCREEN* curr_src);
//initialize the window arrays for the main screen - mainly the windows from the cx in the current cx
int curr_screen_load_win_arrays(APP_INTRF* app_intrf, CURR_SCREEN* curr_src);
//free memory of the main screen
void curr_screen_free(CURR_SCREEN* curr_src);
//refresh all the windows in the main screen. this draws the boxes, different boarders depending on what is
//selected etc.
//if clicked = 0 refresh only the windows that have anim = 1
void curr_screen_refresh(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, unsigned int clicked);
//erase all windows so we can draw next
//if clicked = 0 clear only the windows that have anim = 1
void curr_screen_clear(CURR_SCREEN* curr_scr, unsigned int clicked);
//read user input and react
int curr_screen_read(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr);
//turn off highlights of the current screen
void curr_screen_highlights_off(CURR_SCREEN* curr_scr);
//Layout the windows in their parent_win, or just relative to each other if parent_win is NULL
//list==0 - put in a grid, list==1 - vertical list, list==2 - horizontal list. Lists uses the win_layout_line function,
//grid uses the win_layout_grid function.
//start - is the number in win_array from which to start the display of windows
//returns <0 if there was an error, 0 if all windows fit and number in the win_array that is displayed last
//if the windows do not fit
int win_layout_win_array(WIN* win_array[], unsigned int size, unsigned int list, WIN* parent_win,
			  unsigned int start);
//layout win_array as a line, layout == 1 - vertical line; layout == 2 - horizontal line.
//the widths and heights of the windows are equalized so they resize proportionaly if the parent window is
//resized
int win_layout_line(WIN* win_array[], unsigned int size, int init_y, int init_x, int max_width, int max_height,
			unsigned int start, unsigned int layout);
//layout win_array as a grid. Windows are placed in a grid with a fixed sized (in win->height and win->width
//parameters). If the windows dont fit they are not resized, but scrolling is implemented.
int win_layout_grid(WIN* win_array[], unsigned int size, int init_y, int init_x,
		     int max_width, int max_height, unsigned int start);
//turn of the highlights for the windows in the win array
void win_highlights_off_array(WIN* win_array[], unsigned int size);
//clear windows in a window array, so we can refresh them
//all - if we need to clear all windows or just the animated ones
void win_clear_win_array(WIN* win_array[], unsigned int size, unsigned int all);
//refresh all windows in the window array, draw boxes display the window display_text, etc.
//if all == 1 refresh all windows, if == 0 refresh only win with anim = 1
//if draw_box == NULL draw_box_all will be used for all windows to draw box or no
void win_refresh_win_array(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, WIN* win_array[],
			   unsigned int size, unsigned int* draw_box, unsigned int draw_box_all, unsigned int all);
//initiate win_array, load cx from cx_array
//height_array and width_array - what should be the heights and widths for the windows
//cx_array - context array, can be NULL or partially filled with NULLS
//name_widths - if 1 - width of the windows should be the strlen of the cx name if there is a cx name
//if name_widths is more than 1 - widths should be strlen of the cx name unless the strlen is more than
//name_widths
//name_from_cx - should the window name be taken from the cx in the cx_array
//can_create_children - should the window be able to create windows in the children_array,
//this is relevant for example for windows that have parameter name and value windows in their children_array.
//other windows can send NULL instead of this array
WIN** win_init_win_array(APP_INTRF* app_intrf,  WIN* parent_win, unsigned int size, CX* cx_array[],
			 unsigned int height_array[], unsigned int width_array[], unsigned int name_widths, unsigned int name_from_cx[],
			 unsigned int can_create_children[]);
//clear memory of a window array
void win_free_win_array(WIN* win_array[], unsigned int size);
//clear memory of a window struct
void win_free(WIN* win);
//refresh a single window - display its display_text, draw box etc. If highlight == 1,
//draw this window as highlighted
void win_refresh(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, WIN* win, unsigned int draw_box, unsigned int highlight);
//create a new window struct, initialize its members
//can_create_children - windows like those that hold parameters, can create other windows in their children_array
//since the parent and windows in children_arrays have the same cx_obj, children can pass the condition when
//to create windows in children_array and an infinite recursion would occur, this is why this var exists
int win_create(APP_INTRF* app_intrf, WIN* app_win, int height, int width, int starty, int startx, int highlight,
	       const char* text, CX* cx_obj, unsigned int can_create_children);
//function to change the window display_text, accepts arguments like printf
void win_change_text(WIN* app_win, const char* in_text, ...);
//function that returns a win that has been clicked
WIN* ctrl_win_return_clicked(CURR_SCREEN* curr_scr, int y, int x);
//function that scrolls the given win_array (in essence changes the start variable to the new one)
//win_array the window array that does not fit into the parent window, win_parent - parent window,
//start - reference to the start window in the array, size - array size,
//list - the variable from win_layout_win_array, up - scroll up (1) or down (0)
void ctrl_win_array_scroll(WIN* win_array[], WIN* win_parent, unsigned int* start, unsigned int size,
			   unsigned int list, unsigned int up);

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
        log_append_logfile("%s\n", err);
        exit(1);
    }
    //if error occured but the app_intrf initialized, parse the error and continue
    if(intrf_status<0){
        const char* err = app_intrf_write_err(&intrf_status);
        log_append_logfile("%s\n", err);
    }

    //start ncurses interface
    initscr();
    //cbreak();
    clear();
    //raw();
    curs_set(0);
    noecho();
    //use mouse
    mousemask(ALL_MOUSE_EVENTS, NULL);
    //wait for user input but continue if no input used
    //halfdelay(1);
    //the screen struct that will hold the infor for the current screen, like cx arrays and so forth
    CURR_SCREEN curr_scr;
    if(curr_screen_init(app_intrf, &curr_scr)<0)exit(1);
    curr_screen_refresh(app_intrf, &curr_scr, 1);
    nodelay(curr_scr.curr_main_win->nc_win, 1);
    while(1){
	curr_screen_refresh(app_intrf, &curr_scr, 1);	
	//navigate the screen, if returns 1 exit the program
	if(curr_screen_read(app_intrf, &curr_scr)==1)break;
	curr_screen_clear(&curr_scr, 1);
	//read the lines from the logfile if there are new ones in there
	//TODO would be better to use a void pointer with a string to display instead of opening a file constantly
	if(curr_scr.tick %2000==0){
	    unsigned int curr_line = log_calclines_logfile();
	    if((curr_line-1)>=curr_scr.log_line){
		char* log_text = log_getline_logfile(curr_scr.log_line);
		if(log_text!=NULL){
		    win_change_text(curr_scr.title_scroll_win[0], log_text);
		    free(log_text);
		}
		curr_scr.log_line += 1;
	    }
	}
	nav_update_params(app_intrf);
    }
    curr_screen_free(&curr_scr);
    
    endwin();
    return 0;
}

//prepare the current screen struct, by filling its members
int curr_screen_init(APP_INTRF* app_intrf, CURR_SCREEN* curr_src){
    if(!app_intrf)return -1;
    if(!curr_src)return -1;
    curr_src->tick = 0;
    curr_src->log_line = 0;
    curr_src->win_array = NULL;
    curr_src->win_array_size = 0;
    curr_src->win_array_start = 0;
    curr_src->win_array_end = 0;
    curr_src->button_win_array = NULL;
    curr_src->button_win_array_size = 0;
    curr_src->root_win_array = NULL;
    curr_src->root_win_array_size = 0;    
    curr_src->title_win = NULL;
    curr_src->title_lr_win = NULL;
    curr_src->main_win = NULL;
    curr_src->title_scroll_win = NULL;
    curr_src->scroll_bar_wins = NULL;
    //init the title window
    WIN* title_win = (WIN*)malloc(sizeof(WIN));
    if(!title_win)return -1;
    curr_src->title_win = title_win;
    if(win_create(app_intrf, title_win, 3, COLS-1, 0, 0, 0, NULL, NULL, 0)!=0)return-1;
    title_win->cx_obj = NULL;
    
    //init the main window
    WIN* main_win = (WIN*)malloc(sizeof(WIN));
    if(!main_win)return -1;
    curr_src->main_win = main_win;
    if(win_create(app_intrf, main_win, LINES-9, COLS-1, 0, 0, 0, NULL, NULL, 0)!=0)return -1;    
    main_win->cx_obj = NULL;

    //init the button window
    WIN* but_win = (WIN*)malloc(sizeof(WIN));
    if(!but_win)return -1;
    curr_src->button_win = but_win;
    if(win_create(app_intrf, but_win, 3, COLS-1, 0, 0, 0, NULL, NULL, 0)!=0)return -1;
    but_win->cx_obj = NULL;

    //init the fast navigation window
    WIN* fast_win = (WIN*)malloc(sizeof(WIN));
    if(!fast_win)return -1;
    curr_src->fast_win = fast_win;
    if(win_create(app_intrf, fast_win, 3, COLS-1, 0, 0, 0, NULL, NULL, 0)!=0)return -1;
    fast_win->cx_obj = NULL;
    
    //set the currently selected window
    curr_src->curr_main_win = main_win;
    
    //init the win arrays
    if(curr_screen_load_win_arrays(app_intrf, curr_src)<0)return -1;   
    
    return 0;
}

int curr_screen_load_win_arrays(APP_INTRF* app_intrf, CURR_SCREEN* curr_src){
    if(!app_intrf)return -1;
    if(!curr_src)return -1;
    CX* curr_cx = nav_ret_curr_cx(app_intrf);
    if(!curr_cx)return -1;

    unsigned int total = 0;
    //create the title_lr window array
    //if it does not exist create both title windows
    if(curr_src->title_lr_win == NULL){
	CX** title_array = (CX**)malloc(sizeof(CX*)*2);
	title_array[0] = curr_cx;
	title_array[1] = NULL;
	curr_src->title_lr_win = win_init_win_array(app_intrf, curr_src->title_win, 2, title_array,
						    NULL, (unsigned int[2]){20, 40}, 0, (unsigned int[2]){1,0}, NULL);
	if(!curr_src->title_lr_win)return -1;
	if(title_array)free(title_array);
	//create the array that will hold the title window and the scoll button array
	if(curr_src->title_scroll_win == NULL){
	    curr_src->title_scroll_win = win_init_win_array(app_intrf, curr_src->title_lr_win[1], 2, NULL,
						       NULL, (unsigned int[2]){20, 20}, 0, NULL, NULL);
	    if(!curr_src->title_scroll_win)return -1;
	}
    }   
    //if it exists only create the current cx title window - we need the right title window to be persistent
    else{
	win_free(curr_src->title_lr_win[0]);
	curr_src->title_lr_win[0] = (WIN*)malloc(sizeof(WIN));
	if(!curr_src->title_lr_win[0])return -1;
	const char* curr_cx_name = nav_get_cx_name(app_intrf, curr_cx);
	win_create(app_intrf, curr_src->title_lr_win[0], 3, 20, 0, 0, 0, curr_cx_name, curr_cx, 0);
    }
    if(curr_src->title_lr_win[0]){
	//write type for the title window
	curr_src->title_lr_win[0]->win_type = Title_win_type;
    }
    total = 0;
    
    //the scroll button array
    if(curr_src->scroll_bar_wins == NULL){
	curr_src->scroll_bar_wins = win_init_win_array(app_intrf, curr_src->title_scroll_win[1], 2, NULL,
							  NULL, NULL, 0, NULL, NULL);
	if(!curr_src->scroll_bar_wins)return -1;
	//write the type for the scroll buttons
	curr_src->scroll_bar_wins[0]->win_type = (Scroll_win_type | Scroll_up_win_type);
	curr_src->scroll_bar_wins[1]->win_type = (Scroll_win_type | Scroll_down_win_type);
    }
    total = 0;
   
    //create the main cx array, shown in the main_win
    // if there is an array already free it first
    if(curr_src->win_array){
	win_free_win_array(curr_src->win_array, curr_src->win_array_size);
	curr_src->win_array = NULL;
	curr_src->win_array_size = 0;
	curr_src->win_array_start = 0;	
    }
    CX** cx_array_main = nav_return_children(app_intrf, curr_cx, &total, 1);
    if(total>0){
	unsigned int widths[total];
	unsigned int name_from_cx[total];
	unsigned int create_children[total];
	//new total will be calculated because some contexts might need to be completely hidden
	//and not even calculated, for example output ports when they are not of the current context
	unsigned int new_total = 0;
	CX** cx_new_array_main = malloc(sizeof(CX*) * total);
	for(int i = 0; i<total; i++){
	    if(nav_return_need_to_highlight(cx_array_main[i])==2)continue;
	    widths[new_total] = 21;
	    name_from_cx[new_total] = 1;
	    create_children[new_total] = 1;
	    cx_new_array_main[new_total] = cx_array_main[i];
	    new_total += 1;
	}
	curr_src->win_array = win_init_win_array(app_intrf, curr_src->main_win, new_total, cx_new_array_main,
						 NULL, widths, 0, name_from_cx, create_children);
	if(!curr_src->win_array)return -1;
	curr_src->win_array_size = new_total;	
	if(cx_new_array_main)free(cx_new_array_main);
    }
    if(cx_array_main)free(cx_array_main);
   
    //create the button cx array, shown in the button_win
    if(curr_src->button_win_array){
	win_free_win_array(curr_src->button_win_array, curr_src->button_win_array_size);
	curr_src->button_win_array = NULL;
	curr_src->button_win_array_size = 0;
    }
    total = 0;
    CX** cx_array_button = nav_return_children(app_intrf, curr_cx, &total, 2);
    if(total>0){
	unsigned int name_from_cx[total];
	unsigned int create_children[total];	
	for(int i=0; i<total;i++){
	    name_from_cx[i] = 1;
	    create_children[i] = 1;
	}
	curr_src->button_win_array_size = total;
	curr_src->button_win_array = win_init_win_array(app_intrf, curr_src->button_win, total, cx_array_button,
							NULL, NULL, 20, name_from_cx, create_children);
	if(!curr_src->button_win_array)return -1;
    }
    if(cx_array_button)free(cx_array_button);

    //create the root cx array, shown in the fast_win
    total = 0;
    if(!curr_src->root_win_array){
	CX* root_cx = nav_ret_root_cx(app_intrf);
	if(!root_cx)return -1;

	CX** cx_array_root = nav_return_children(app_intrf, root_cx, &total, 1);
	if(total>0){
	    unsigned int name_from_cx[total];
	    unsigned int create_children[total];
	    for(int i=0; i<total;i++){
		name_from_cx[i] = 1;
		create_children[i] = 1;
	    }
	    curr_src->root_win_array_size = total;
	    curr_src->root_win_array = win_init_win_array(app_intrf, curr_src->fast_win, total, cx_array_root,
							  NULL, NULL, 20, name_from_cx, create_children);
	    if(!curr_src->root_win_array)return -1;
	}
	if(cx_array_root)free(cx_array_root);
    }
    
    return 0;
}

void curr_screen_free(CURR_SCREEN* curr_src){
    if(!curr_src)return;
    //free the scroll bar
    if(curr_src->scroll_bar_wins){
	win_free_win_array(curr_src->scroll_bar_wins, 2);
	curr_src->scroll_bar_wins = NULL;
    }
    //free the title scroll window that holds the info and scroll bar
    if(curr_src->title_scroll_win){
	win_free_win_array(curr_src->title_scroll_win, 2);
	curr_src->title_scroll_win = NULL;
    }    
    //free the title_lr array
    if(curr_src->title_lr_win){
	win_free_win_array(curr_src->title_lr_win, 2);
	curr_src->title_lr_win = NULL;
    }
    //free the main win cx array
    if(curr_src->win_array){
	win_free_win_array(curr_src->win_array, curr_src->win_array_size);
	curr_src->win_array = NULL;
	curr_src->win_array_size = 0;
    }
    //free the button win cx array
    if(curr_src->button_win_array){
	win_free_win_array(curr_src->button_win_array, curr_src->button_win_array_size);
	curr_src->button_win_array = NULL;
	curr_src->button_win_array_size = 0;
    }
    //free the root win cx array for bottom navigation
    if(curr_src->root_win_array){
	win_free_win_array(curr_src->root_win_array, curr_src->root_win_array_size);
	curr_src->root_win_array = NULL;
	curr_src->root_win_array_size = 0;
    }
    
    if(curr_src->title_win)win_free(curr_src->title_win);
    if(curr_src->main_win)win_free(curr_src->main_win);
    if(curr_src->button_win)win_free(curr_src->button_win);
    if(curr_src->fast_win)win_free(curr_src->fast_win);
}

void curr_screen_refresh(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, unsigned int clicked){
    if(!curr_scr)return;
    if(!curr_scr->title_win)return;
    if(!curr_scr->main_win)return;
    if(!curr_scr->button_win)return;
    if(!curr_scr->fast_win)return;
    //before refreshing unhighlight all the windows if there are any
    curr_screen_highlights_off(curr_scr);
    //increase the tick count and reset if necessary
    curr_scr->tick += 1;
    if(curr_scr->tick>MAX_TICK)curr_scr->tick = 0;  
    //layout the main windows, so they are correctly drawn even if the terminal is resized
    WIN* main_win_array[4] = {curr_scr->title_win, curr_scr->main_win, curr_scr->button_win, curr_scr->fast_win};
    win_layout_win_array(main_win_array, 4, 1, NULL, 0);
    //refresh the main windows
    win_refresh_win_array(app_intrf, curr_scr, main_win_array, 4, NULL, 0, clicked);
    //layout and refresh the title_lr windows
    win_layout_win_array(curr_scr->title_lr_win, 2, 2, curr_scr->title_win, 0);
    win_refresh_win_array(app_intrf, curr_scr, curr_scr->title_lr_win, 2, (unsigned int [2]){1, 0}, 0, clicked);
    //layout the title_scroll_win that holds the info on the left and scroll bars on the right
    win_layout_win_array(curr_scr->title_scroll_win, 2, 2, curr_scr->title_lr_win[1], 0);
    win_refresh_win_array(app_intrf, curr_scr, curr_scr->title_scroll_win, 2, (unsigned int [2]){1,0}, 0, clicked);   
    //layout the main window cx array
    int scroll = win_layout_win_array(curr_scr->win_array, curr_scr->win_array_size, 0, curr_scr->main_win,
				      curr_scr->win_array_start);
    //if the win_array items do not fit implement scrolling - show the scroll buttons
    if(scroll >= 0 || curr_scr->win_array_start>0){
	curr_scr->win_array_end = scroll;
	curr_scr->scroll_bar_wins[1]->hide = 0;
	curr_scr->scroll_bar_wins[0]->hide = 0;
	//layout the title_scroll_win scroll button array
	win_layout_win_array(curr_scr->scroll_bar_wins, 2, 2, curr_scr->title_scroll_win[1], 0);
	//if the start item is 0 we dont need the scroll bar that scrolls up
	if(curr_scr->win_array_start==0)
	    curr_scr->scroll_bar_wins[0]->hide = 1;
    }
    //if all context in win_array fit hide the main window scroll buttons, we do this after laying them down
    //since if we would lay them out after this conditional the layout would simply unhide these windows again
    if(scroll == -1){
	curr_scr->win_array_end = curr_scr->win_array_size-1;
	//only hide the up scroll bar if the start item is 0, otherwise it means there are some previous items
	//and the user has to be able scroll up(or back, how ever you look at it)
	if(curr_scr->win_array_start==0)
	    curr_scr->scroll_bar_wins[0]->hide = 1;
	curr_scr->scroll_bar_wins[1]->hide = 1;
    }
    if(curr_scr->scroll_bar_wins[0]->hide!=0 && curr_scr->scroll_bar_wins[1]->hide!=0){
	curr_scr->title_scroll_win[1]->hide = 2;
    }
    else{
	curr_scr->title_scroll_win[1]->hide = 0;
    }
 
    //now refresh the main window win arrays, we do it here because we hide and unhide them
    win_refresh_win_array(app_intrf, curr_scr, curr_scr->scroll_bar_wins, 2, NULL, 1, clicked);    
    win_refresh_win_array(app_intrf, curr_scr, curr_scr->win_array, curr_scr->win_array_size, NULL, 1, clicked);

    //layout and refresh the button window cx array
    win_layout_win_array(curr_scr->button_win_array, curr_scr->button_win_array_size, 2,
			 curr_scr->button_win, 0);
    win_refresh_win_array(app_intrf, curr_scr, curr_scr->button_win_array, curr_scr->button_win_array_size, NULL, 1, clicked);
    //layout and refresh the root window cx array
    win_layout_win_array(curr_scr->root_win_array, curr_scr->root_win_array_size, 2, curr_scr->fast_win, 0);
    win_refresh_win_array(app_intrf, curr_scr, curr_scr->root_win_array, curr_scr->root_win_array_size, NULL, 1, clicked);
    doupdate();
}

void curr_screen_clear(CURR_SCREEN* curr_scr, unsigned int clicked){
    if(!curr_scr)return;
    if(!curr_scr->title_win)return;    
    if(!curr_scr->main_win)return;
    if(!curr_scr->button_win)return;
    if(!curr_scr->fast_win)return;
    //clear the main windows
    WIN* main_win_array[4] = {curr_scr->title_win, curr_scr->main_win, curr_scr->button_win, curr_scr->fast_win}; 
    win_clear_win_array(main_win_array, 4, clicked);
    //clear the main array scroll array
    win_clear_win_array(curr_scr->scroll_bar_wins, 2, clicked);
    //clear the title_scroll_win that holds the info on the left and the scroll bar on the right
    win_clear_win_array(curr_scr->title_scroll_win, 2, clicked);
    //clear the title_lr array
    win_clear_win_array(curr_scr->title_lr_win, 2, clicked);
    //clear the main cx array
    win_clear_win_array(curr_scr->win_array, curr_scr->win_array_size, clicked);
    //clear the button cx array
    win_clear_win_array(curr_scr->button_win_array, curr_scr->button_win_array_size, clicked);
    //clear the root cx array
    win_clear_win_array(curr_scr->root_win_array, curr_scr->root_win_array_size, clicked);
}
//enter the cx of the window if there is a cx_obj
static int curr_enter_window(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, WIN* clicked_win){
    CX* cur_cx = clicked_win->cx_obj;
    if(cur_cx){
	//set the selected context to the clicked context
	nav_set_select_cx(app_intrf, cur_cx);
	//invoke can destroy context, for ex when entering directories
	//thats why we need to use the returned cx to enter
	CX* sel_cx = nav_invoke_cx(app_intrf, cur_cx);
	nav_enter_cx(app_intrf, sel_cx);	    
	if(curr_screen_load_win_arrays(app_intrf, curr_scr)<0)return -1;
    }
    return 0;
}
//interpret the keypress codes and do the correct navigation action
//TODO keypresses should be in a json file so the user can modify the shortcuts
static int curr_input_keypress_read(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, int ch, WINDOW* main_window){
    if(!app_intrf || !curr_scr)return 0;
    int ret_val = 0;
    switch(ch){
    case '1':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '2':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+1){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+1];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '3':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+2){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+2];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '4':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+3){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+3];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '5':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+4){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+4];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '6':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+5){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+5];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '7':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+6){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+6];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case '8':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+7){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+7];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;		
	//alt key with numbers pressed, using code of the M-1, M-2...etc here
    case 177:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 178:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+1){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+1];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 179:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+2){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+2];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 180:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+3){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+3];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 181:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+4){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+4];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 182:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+5){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+5];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 183:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+6){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+6];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
    case 184:
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+7){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+7];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, -1);
	}
	break;
	//shift + 1 was pressed
    case '!':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 2 was pressed
    case '@':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+1){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+1];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 3 was pressed
    case '#':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+2){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+2];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 4 was pressed
    case '$':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+3){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+3];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 5 was pressed
    case '%':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+4){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+4];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 6 was pressed
    case '^':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+5){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+5];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 7 was pressed
    case '&':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+6){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+6];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
	//shift + 8 was pressed
    case '*':
	if(curr_scr->win_array && curr_scr->win_array_end >= curr_scr->win_array_start+7){
	    WIN* first_win = curr_scr->win_array[curr_scr->win_array_start+7];
	    if(first_win)
		nav_set_cx_value(app_intrf, first_win->cx_obj, 1);
	}
	break;
    case'-':
	//scroll the main context array up - the last window displayed + 1 will become first
	ctrl_win_array_scroll(curr_scr->win_array, curr_scr->main_win,
			      &(curr_scr->win_array_start), curr_scr->win_array_size, 0, 1);
	break;
    case'+':
	//scroll the main context array down - the first window displayed - 1 will become last
	ctrl_win_array_scroll(curr_scr->win_array, curr_scr->main_win,
			      &(curr_scr->win_array_start), curr_scr->win_array_size, 0, 0);
	break;

	//navigate the button array (liek save, load etc)
    case'w':
	if(curr_scr->button_win_array && curr_scr->button_win_array_size > 0){
	    WIN* first_win = curr_scr->button_win_array[0];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'e':
	if(curr_scr->button_win_array && curr_scr->button_win_array_size > 1){
	    WIN* first_win = curr_scr->button_win_array[1];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'r':
	if(curr_scr->button_win_array && curr_scr->button_win_array_size > 2){
	    WIN* first_win = curr_scr->button_win_array[2];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case't':
	if(curr_scr->button_win_array && curr_scr->button_win_array_size > 3){
	    WIN* first_win = curr_scr->button_win_array[3];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'y':
	if(curr_scr->button_win_array && curr_scr->button_win_array_size > 4){
	    WIN* first_win = curr_scr->button_win_array[4];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'u':
	if(curr_scr->button_win_array && curr_scr->button_win_array_size > 5){
	    WIN* first_win = curr_scr->button_win_array[5];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;	
	//navigate the root array (the main contexts that we see at the bottom all the time)
    case's':
	if(curr_scr->root_win_array && curr_scr->root_win_array_size > 0){
	    WIN* first_win = curr_scr->root_win_array[0];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'd':
	if(curr_scr->root_win_array && curr_scr->root_win_array_size > 1){
	    WIN* first_win = curr_scr->root_win_array[1];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'f':
	if(curr_scr->root_win_array && curr_scr->root_win_array_size > 2){
	    WIN* first_win = curr_scr->root_win_array[2];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'g':
	if(curr_scr->root_win_array && curr_scr->root_win_array_size > 3){
	    WIN* first_win = curr_scr->root_win_array[3];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'h':
	if(curr_scr->root_win_array && curr_scr->root_win_array_size > 4){
	    WIN* first_win = curr_scr->root_win_array[4];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;
    case'j':
	if(curr_scr->root_win_array && curr_scr->root_win_array_size > 5){
	    WIN* first_win = curr_scr->root_win_array[5];
	    if(first_win)
		ret_val = curr_enter_window(app_intrf, curr_scr, first_win);
	}
	break;	
    case 'q':
	int cx_exit = nav_exit_cur_context(app_intrf);
	if(cx_exit == -2)return 1;
	if(curr_screen_load_win_arrays(app_intrf, curr_scr)<0)return -1;		
	break;
	
    default:
	break;
    }
    return ret_val;
}
//read the input and navigate the screen, returns 1 if we need to exit the program
int curr_screen_read(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr){
    if(!curr_scr)return -1;
    if(!curr_scr->curr_main_win)return -1;    
    WINDOW* main_window = curr_scr->curr_main_win->nc_win;
    if(!main_window)return-1;
    keypad(main_window, TRUE);
    int ret_val = 0;
    int ch = '\0';
    //read the input char
    MEVENT event;
    ch = wgetch(main_window);
    if(ch == KEY_MOUSE){
	if(getmouse(&event) == OK){
	    if(event.bstate & BUTTON1_CLICKED){
		WIN* clicked_win = ctrl_win_return_clicked(curr_scr, event.y, event.x);
		if(clicked_win!=NULL){
		    //if clicked the title window exit the current cx
		    if(clicked_win->win_type == Title_win_type){
			if(nav_exit_cur_context(app_intrf) == -2)return 1;
			if(curr_screen_load_win_arrays(app_intrf, curr_scr)<0)return -1;
		    }
		    //if clicked on scroll up button window
		    else if(clicked_win->win_type == (Scroll_win_type | Scroll_up_win_type)){
			ctrl_win_array_scroll(curr_scr->win_array, curr_scr->main_win,
					      &(curr_scr->win_array_start), curr_scr->win_array_size, 0, 1);
		    }
		    //if clicked on scroll down button window
		    else if(clicked_win->win_type == (Scroll_win_type | Scroll_down_win_type)){
			//scroll the main context array down - the last window displayed + 1 will become first
			ctrl_win_array_scroll(curr_scr->win_array, curr_scr->main_win,
					      &(curr_scr->win_array_start), curr_scr->win_array_size, 0, 0);
		    }
		    //if the clicked window is a paramater and user clicked on parameter name - decrease its value
		    else if(clicked_win->win_type == (Param_win_type | Param_name_win_type)){
			nav_set_cx_value(app_intrf, clicked_win->cx_obj, -1);
		    }
		    //if the clicked window is a paramater and user clicked on parameter value - increase its value
		    else if(clicked_win->win_type == (Param_win_type | Param_val_win_type)){
			nav_set_cx_value(app_intrf, clicked_win->cx_obj, 1);
		    }
		    else{
			if(curr_enter_window(app_intrf, curr_scr, clicked_win)<0)return -1;
		    }
		}
	    }
	    //check if double clicked, mainly to increase/decrease params more
	    if(event.bstate & BUTTON1_DOUBLE_CLICKED){
		WIN* clicked_win = ctrl_win_return_clicked(curr_scr, event.y, event.x);
		if(clicked_win!=NULL){
		    //if the clicked window is a paramater and user clicked on parameter name - decrease its value
		    if(clicked_win->win_type == (Param_win_type | Param_name_win_type)){
			nav_set_cx_value(app_intrf, clicked_win->cx_obj, -10);
		    }
		    //if the clicked window is a paramater and user clicked on parameter value - increase its value
		    else if(clicked_win->win_type == (Param_win_type | Param_val_win_type)){
			nav_set_cx_value(app_intrf, clicked_win->cx_obj, 10);
		    }		    
		}
	    }
	}
    }
    //check keystrokes and key combinations
    else ret_val = curr_input_keypress_read(app_intrf, curr_scr, ch, main_window);
    return ret_val;
}

void curr_screen_highlights_off(CURR_SCREEN* curr_scr){
    if(!curr_scr)return;
    WIN* main_array[4];
    main_array[0] = curr_scr->title_win;
    main_array[1] = curr_scr->button_win;
    main_array[2] = curr_scr->fast_win;
    main_array[3] = curr_scr->main_win;
    win_highlights_off_array(main_array, 4);
    win_highlights_off_array(curr_scr->button_win_array, curr_scr->button_win_array_size);
    win_highlights_off_array(curr_scr->root_win_array, curr_scr->root_win_array_size);
    win_highlights_off_array(curr_scr->title_lr_win, 2);
    win_highlights_off_array(curr_scr->win_array, curr_scr->win_array_size);
}

int win_layout_win_array(WIN* win_array[], unsigned int size, unsigned int list, WIN* parent_win,
			  unsigned int start){
    if(!win_array)return -1;
    WIN* new_array[size];
    //build a new array, that does not contain windows with hide==2
    unsigned int array_start = start;
    unsigned int array_size = size;

    unsigned int iter = 0;
    unsigned int old_array_iter = 0;
    unsigned int new_size = 0;
    int new_start = -1;
    while(old_array_iter<size){
	WIN* cur_win = win_array[old_array_iter];
	if(cur_win->hide!=2){
	    new_array[iter] = cur_win;
	    new_size += 1;
	    if(old_array_iter==start)new_start=iter;		
	    iter += 1;
	}
	old_array_iter += 1;
    }
    if(new_start == -1) new_start = 0;
    array_start = new_start;
    array_size = new_size;
    
    int y = 0;
    int x = 0;
    int max_width = COLS;
    int max_height = LINES;
    if(parent_win && parent_win->hide!=2){
	y = parent_win->win_y;
	x = parent_win->win_x;
	getmaxyx(parent_win->nc_win, max_height, max_width);
    }
    int scroll = -1;
    if(array_size>0){
	//scroll is implemented only for the grid layout so others will return -1
	if(list==1)win_layout_line(new_array, array_size, y, x, max_width, max_height, array_start, 1);
	if(list==2)win_layout_line(new_array, array_size, y, x, max_width, max_height, array_start, 2);
	if(list==0)scroll = win_layout_grid(new_array, array_size, y, x, max_width, max_height, array_start);
    }
    //layout windows array children if there are any
    for(int i = 0; i<array_size; i++){
	WIN* cur_win = new_array[i];
	if(!cur_win)continue;
	if(cur_win->children_array_size>0){
	    //layout the children, now the children can only be layed out in a line
	    win_layout_win_array(cur_win->children_array, cur_win->children_array_size, 2, cur_win, 0);
	}
    }
    return scroll;
}

static int array_equalize(int int_array[], unsigned int size, int max){
    int sum = 0;
    for(int i=0; i<size; i++){
	int cur = int_array[i];
	sum+=cur;
    }
    if(sum>max){
	sum = 0;
	for(int i=0; i<size; i++){
	    int cur = int_array[i];
	    if(cur>3)cur-=1;
	    int_array[i] = cur;
	    sum+=cur;
	}
	if(sum>(size*3) && sum>max)array_equalize(int_array, size, max);
    }
    return sum;
}

int win_layout_line(WIN* win_array[], unsigned int size, int init_y, int init_x, int max_width, int max_height,
			unsigned int start, unsigned int layout){
    int cur_y = init_y;
    int cur_x = init_x;
    //calculate the total height and width in the win_array;
    int total_height = 0;
    int total_width = 0;    
    for(int i=0; i<size; i++){
	WIN* cur_win = win_array[i];
	int cur_height = cur_win->height;
	total_height += cur_height;
	int cur_width = cur_win->width;
	total_width += cur_width;	
    } 
    if(total_height<=0)return -1;
    if(total_width<=0) return -1;
    int scroll = -1;
    //we need to equalize the heights of the windows, so they get smaller equally if they dont fit
    int heights[size];
    int widths[size];    
    for(int i=0; i<size; i++){
	int cur_height = win_array[i]->height;
	int cur_width = win_array[i]->width;	
	float height_percent = (((float)cur_height*100.0)/(float)total_height)*0.01;
	float width_percent = (((float)cur_width*100.0)/(float)total_width)*0.01;	
	heights[i] = max_height*height_percent;
	widths[i] = max_width*width_percent;	
	if(heights[i]<3)heights[i] = 3;
	if(widths[i]<3)widths[i] = 3;	
    }
    array_equalize(heights, size, max_height);
    array_equalize(widths, size, max_width);
    
    int window_count = 0;
    int sum_heights = 0; 
    int sum_widths = 0;   
    for(int i=0; i<size; i++){
	WIN* cur_win = win_array[i];
	int cur_height = cur_win->height;
	if(cur_height>max_height)cur_height = max_height;
	if(layout==1){
	    cur_height = heights[i];
	    sum_heights += cur_height;
	}
	int cur_width = cur_win->width;
	if(cur_width>max_width)cur_width = max_width;
	if(layout==2){
	    cur_width = widths[i];
	    sum_widths += cur_width;
	}
	window_count += 1;
	cur_win->hide = 0;
	wresize(cur_win->nc_win, cur_height, cur_width);
	int assign_y = init_y;
	int assign_x = init_x;
	if(layout==1)assign_y = cur_y;
	if(layout==2)assign_x = cur_x;
	    
	cur_win->win_y = assign_y;
	cur_win->win_x = assign_x;
	mvwin(cur_win->nc_win, assign_y, assign_x);
	cur_y += cur_height;
	cur_x += cur_width;
    }  
    return scroll;
}

int win_layout_grid(WIN* win_array[], unsigned int size, int init_y, int init_x,
		     int max_width, int max_height, unsigned int start){
    //calc average width and height
    int total_width = 0;
    int total_height = 0;
    if(start>=size)return -1;
    for(int i=start; i<size; i++){
	WIN* cur_win = win_array[i];
	CX* cur_cx = cur_win->cx_obj;
	if(!cur_win)continue;
	int cur_width = cur_win->width;
	int cur_height = cur_win->height;
	total_width+=cur_width;
	total_height+=cur_height;
    }
    if(total_height<=0 || total_width<=0)return -1;
    int sub = size-start;
    if(sub<=0)sub = 1;
    int avr_width = total_width/sub;
    if(avr_width>max_width)avr_width = max_width;
    int avr_height = total_height/sub;
    if(avr_height>max_height)avr_height = max_height;

    //how many windows fit with avr_width and avr_heigth
    if(avr_width<=0)avr_width = 1;
    if(avr_height<=0)avr_height = 1;
    int fit_width =  max_width/avr_width;
    int fit_height = max_height/avr_height;

    int cur_y = init_y;
    int cur_x = init_x;
    int cur_width_num = 0;
    int cur_height_num = 0;
    int scroll = -1;
    //iterator to count how many windows there will be actually displayed
    unsigned int displayed_windows = 0;
    for(int i=0; i<size; i++){
	WIN* cur_win = win_array[i];
	if(!cur_win)continue;
	//if we start to show windows not from the first item in the array, highlight the first window we will
	//show, to indicate to the user that there are some windows in the previous page
	if(start>0 && i == start){
	    cur_win->highlight = 1;
	}
	//if no more windows fit make the other windows in array hidden
	if(scroll >= 0 || i<start){
	    cur_win->hide = 1;
	    continue;
	}
	//if the windows fit unhide the window, if they where previously hidden for example
	else{
	    cur_win->hide = 0;
	}
	displayed_windows += 1;
	
	wresize(cur_win->nc_win, avr_height, avr_width);
	cur_win->win_y = cur_y;
	cur_win->win_x = cur_x;	
	mvwin(cur_win->nc_win, cur_y, cur_x);
	cur_height_num +=1;
	//check if the windows fit, if not, implement scrolling
	if(cur_height_num>=fit_height){
	    cur_height_num = 0;
	    cur_width_num +=1;
	    //also check if this window is the last window in the array, if it is it means
	    //more windows would not fit, but since there are not any more windows dont implement scrolling
	    if(cur_width_num>=fit_width && i != size -1){
		//highlight the last window that fits to indicate to the user that there are more windows on the
		//next page
		cur_win->highlight = 1;
		scroll = i;
	    }
	}
	//also implement scrolling if there are more windows than the user wishes
	//but like with the size dont implement scrolling if its the last window in the array
	if(displayed_windows >=  MAX_WINDOWS && i != size -1){
	    cur_win->highlight = 1;
	    scroll = i;
	}
	//check if the window needs to be highlighted because of the cx status (like a connected port or selected
	//sample
	if(cur_win->cx_obj){
	    if(nav_return_need_to_highlight(cur_win->cx_obj)==1){
		cur_win->highlight = 2;
	    }

	}
	cur_x = (cur_width_num * avr_width)+init_x;
	cur_y = (cur_height_num * avr_height)+init_y;
    }
    return scroll;
}

void win_highlights_off_array(WIN* win_array[], unsigned int size){
    if(!win_array)return;
    for(unsigned int i=0; i<size; i++){
	WIN* cur_win = win_array[i];
	if(!cur_win)continue;
	cur_win->highlight = 0;
	//if this window has children turn the highlights of for the too
	if(cur_win->children_array_size>0){
	    win_highlights_off_array(cur_win->children_array, cur_win->children_array_size);
	}
    }
}

WIN** win_init_win_array(APP_INTRF* app_intrf,  WIN* parent_win, unsigned int size, CX* cx_array[],
			 unsigned int height_array[], unsigned int width_array[], unsigned int name_widths, unsigned int name_from_cx[],
			 unsigned int can_create_children[]){
    WIN** win_array = (WIN**)malloc(sizeof(WIN*)*size);
    if(!win_array)return NULL;
    for(int i = 0; i<size; i++){
	CX* cur_cx = NULL;
	if(cx_array) cur_cx = cx_array[i];
	WIN* cur_win = (WIN*)malloc(sizeof(WIN));
	if(cur_win){
	    win_array[i] = cur_win;
	}
	int cur_width = 20;
	int cur_height = 3;
	if(height_array){
	    cur_height = height_array[i];
	}
	if(width_array){
	    cur_width = width_array[i];
	}
	unsigned int create_children = 0;
	if(can_create_children){
	    create_children = can_create_children[i];
	}
	unsigned int add_name_from_cx = 1;
	if(!name_from_cx)add_name_from_cx = 0;
	else add_name_from_cx = name_from_cx[i];
	if(cur_cx && add_name_from_cx==1){
	    const char* cx_name = nav_get_cx_name(app_intrf, cur_cx);
	    unsigned int new_width = cur_width;
	    if(name_widths>0){
		unsigned int name_len = strlen(cx_name)+1;
		new_width = name_len;
		if(name_widths!=1){
		    if(name_len>name_widths)new_width = name_widths;
		}
	    }
	    win_create(app_intrf, cur_win, cur_height, new_width, 0, 0, 0, cx_name, cur_cx, create_children);   
	}
	else{
	    win_create(app_intrf, cur_win, cur_height, cur_width, 0, 0, 0, NULL, cur_cx, create_children);
	}
    }

    return win_array;
}

int win_create(APP_INTRF* app_intrf, WIN* app_win, int height, int width, int starty, int startx, int highlight,
	       const char* text, CX* cx_obj, unsigned int can_create_children){
    if(!app_win)return -1;
    app_win->display_text = NULL;
    app_win->scroll_text = NULL;
    app_win->text_start = 0;
    app_win->anim = 0;
    app_win->win_type = 0;
    app_win->children_array = NULL;
    app_win->children_array_size = 0;
    app_win->nc_win = NULL;
    app_win->height = height;
    app_win->width = width;
    app_win->win_x = 0;
    app_win->win_y = 0;
    app_win->hide = 0;
    app_win->highlight = highlight;
    app_win->cx_obj = cx_obj;
    WINDOW* loc_win = NULL;
    loc_win = newwin(height, width, starty, startx);
    if(!loc_win)return -1;
    app_win->nc_win = loc_win;
    if(text){
	app_win->display_text = (char*)malloc(sizeof(char)*(strlen(text)+1));
	if(app_win->display_text){
	    strcpy(app_win->display_text, text);
	}
    }
    //check if this cx is a parameter if yes, create a child_array for this window
    if(can_create_children==1){
	if(cx_obj){
	    unsigned int cx_type = nav_return_cx_type(cx_obj);
	    if((cx_type & 0xff00) == Val_cx_e){
		//this array will contain the cur_cx in the name window and the param value window, so when we click it
		//its easier to send the cx to nav functions to increase or decrease the value.
		CX* cx_param_array[] = {cx_obj, cx_obj};
		app_win->children_array_size = 2;
		app_win->children_array = win_init_win_array(app_intrf, app_win, 2, cx_param_array, NULL, NULL, 0, (unsigned int[2]){1,0}, NULL);
		//put the value of the param as the value window text right away, otherwise when refreshed the text would be empty for a moment
		app_win->children_array[1]->display_text = nav_get_cx_value_as_string(app_intrf, cx_obj);
		//add the win type for the param window

		app_win->children_array[0]->win_type = (Param_win_type | Param_name_win_type);
		app_win->children_array[1]->win_type = (Param_win_type | Param_val_win_type);

	    }
	}
    }

    wrefresh(loc_win);

    return 0;
}

void win_clear_win_array(WIN* win_array[], unsigned int size, unsigned int all){
    if(size<=0)return;
    if(win_array){
	for(int i = 0; i<size; i++){
	    WIN* cur_win = win_array[i];
	    int clear = 1;
	    //dont clear if we need to clear only animated windows
	    if(all == 0 && cur_win->anim == 0)clear = 0;
	    if(cur_win && clear == 1){
		werase(cur_win->nc_win);
		//clear the children of this window if there are any
		if(cur_win->children_array_size>0){
		    win_clear_win_array(cur_win->children_array, cur_win->children_array_size, all);
		}		
	    }
	}	
    }
}

void win_refresh_win_array(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, WIN* win_array[],
			   unsigned int size, unsigned int* draw_box, unsigned int draw_box_all, unsigned int all){
    if(size<=0)return;
    if(win_array){
	for(int i = 0; i<size; i++){
	    WIN* cur_win = win_array[i];    
	    if(cur_win){
		int ref = 1;
		//only refresh window if its not hidden
		if(cur_win->hide!=0)ref = 0;
		//if we need to refresh only animated windows and this one is not do not refresh
		if(cur_win->anim == 0 && all == 0)ref = 0;
		if(ref==1){
		    unsigned int box = 0;
		    if(draw_box)box = draw_box[i];
		    if(!draw_box)box = draw_box_all;
		    if(cur_win->children_array_size<=0)win_refresh(app_intrf, curr_scr, cur_win, box, cur_win->highlight);
		    //if this window has children refresh them too
		    if(cur_win->children_array_size>0){
			//we dont want to draw a box around the parent window so we refresh it again, but draw a box around children
			//if the box should have been drawn around the parent window
			win_refresh(app_intrf, curr_scr, cur_win, 0, cur_win->highlight);
			//if the parent window is highlighet highlight the children too
			for(int ch = 0; ch < cur_win->children_array_size; ch++){
			    WIN* cur_child  = cur_win->children_array[ch];
			    if(!cur_child)continue;
			    cur_child->highlight = cur_win->highlight;
			}
			win_refresh_win_array(app_intrf, curr_scr, cur_win->children_array, cur_win->children_array_size, NULL, box, all);
		    }   
		}
	    }
	}	
    }
}
//draw box depending on the type of window.
//for example for parameter windows we draw a different box to better see them 
static void win_draw_box(WIN* win, unsigned int highlight){
    if(!win)return;
    if(highlight == 0){
	if((win->win_type & 0xff00) == Param_win_type){
	    if((win->win_type & 0x00ff) == Param_name_win_type){
		wborder(win->nc_win, '|', ' ', 0, 0, 0, ' ', 0, ' ');
	    }
	    if((win->win_type & 0x00ff) == Param_val_win_type){
		wborder(win->nc_win, ' ', '|', 0, 0, ' ', 0, ' ', 0);
	    }	    
	}
	else{
	    box(win->nc_win, 0, 0);
	}
    }
    if(highlight == 1 || highlight == 2){
	if((win->win_type & 0xff00) == Param_win_type){
	    if((win->win_type & 0x00ff) == Param_name_win_type){
		wborder(win->nc_win, '|', ' ', '=', '=', '*', ' ', '*', ' ');
	    }
	    if((win->win_type & 0x00ff) == Param_val_win_type){
		wborder(win->nc_win, ' ', '|', '=', '=', ' ', '*', ' ', '*');
	    }	    
	}
	else{
	    wborder(win->nc_win, '|', '|', '=', '=', '*', '*', '*', '*');
	}
    }
}

void win_refresh(APP_INTRF* app_intrf, CURR_SCREEN* curr_scr, WIN* win, unsigned int draw_box, unsigned int highlight){
    if(!win)return;
    if(draw_box==1){
	if(highlight == 0){
	    wattroff(win->nc_win, A_STANDOUT);	    
	    win_draw_box(win, highlight);
	}
	if(highlight == 1){
	    wattroff(win->nc_win, A_STANDOUT);	    
	    win_draw_box(win, highlight);
	}
	if(highlight == 2){
	    wattron(win->nc_win, A_STANDOUT);
	    win_draw_box(win, highlight);
	}
    }
    //if this is a parameter value update it
    //otherwise the value will stay the same as when the window was created
    if(win->cx_obj && win->win_type==(Param_win_type | Param_val_win_type)){
	if(win->display_text){
	    free(win->display_text);
	    win->display_text = NULL;			
	}
	win->display_text = nav_get_cx_value_as_string(app_intrf, win->cx_obj);
    }

    if(win->display_text){
	//display the text but if it does not fit scroll it with the tick
	int max_width = getmaxx(win->nc_win)-2;
	if(max_width < 1) max_width = 1;
	int txt_len = strlen(win->display_text);
	char* display = NULL;
	//if the text fits no need to scroll
	if(txt_len<max_width){
	    display = win->display_text;
	    win->anim = 0;
	}
	//if it does not fit we have to scroll the text
	else{
	    win->anim = 1;
	    unsigned int tick = curr_scr->tick;
	    if(win->scroll_text != NULL){
		display = win->scroll_text;
	    }
	    unsigned int start_char = win->text_start;
	    if(start_char > (strlen(win->display_text)) - max_width){
		start_char = (strlen(win->display_text)) - max_width;
	    }
	    unsigned int end_char = start_char + (max_width-1);
	    if(end_char > strlen(win->display_text)-1){
		end_char = strlen(win->display_text)-1;
	    }
	    else if(tick%1500==0){
		win->text_start += 1;
		if(win->text_start >= strlen(win->display_text))win->text_start = 0;
	    }
	    unsigned int new_len = (end_char - start_char) + 1;
	    
	    //create the new string
	    char* new_text = (char*)malloc(sizeof(char) * (new_len+1));
	    if(!new_text)return;
	    //end null for string
	    new_text[new_len] = '\0';
	    //copy the display text to the new text
	    int j = 0;
	    for(int i=start_char; i<=end_char; i++){
		char cur_char = win->display_text[i];
		new_text[j] = cur_char;
		j += 1;
	    }
	    if(win->scroll_text){
		free(win->scroll_text);
		win->scroll_text = NULL;
	    }
	    win->scroll_text = new_text;
	    display = win->scroll_text;
	}
	if(display){
	    mvwprintw(win->nc_win, 1, 1, display);
	}
    }
 
    wnoutrefresh(win->nc_win);    
}

void win_free_win_array(WIN* win_array[], unsigned int size){
    if(size<=0)return;
    if(win_array){
	for(int i = 0; i<size; i++){
	    WIN* cur_win = win_array[i];
	    if(cur_win){
		win_free(cur_win);
	    }
	}
	free(win_array);
    }    
}

void win_free(WIN* win){
    if(!win)return;
    delwin(win->nc_win);
    if(win->display_text)free(win->display_text);
    win->display_text = NULL;
    if(win->scroll_text)free(win->scroll_text);
    win->scroll_text = NULL;
    //if this win has children free them too
    if(win->children_array_size>0)win_free_win_array(win->children_array, win->children_array_size);
    free(win);
}

void win_change_text(WIN* app_win, const char* in_text, ...){
    char new_string[4096];
    va_list args;
    va_start(args, in_text);
    int string_size = vsnprintf(new_string, sizeof(new_string), in_text, args);
    va_end(args);
    char* temp_string = (char*)malloc(sizeof(char)*(string_size+1));
    if(!temp_string)return;
    strcpy(temp_string, new_string);
    if(app_win->display_text)free(app_win->display_text);
    app_win->display_text = temp_string;
}

static WIN* win_array_clicked(WIN* win_array[], unsigned int array_size, int y, int x){
    WIN* ret_win = NULL;
    if(win_array==NULL)return NULL;
    for(int i = 0; i<array_size; i++){
	WIN* cur_win = win_array[i];
	if(cur_win==NULL)continue;
	//dont click windows that are hidden
	if(cur_win->hide!=0)continue;
	//if this window has other children windows check if they are not clicked
	if(cur_win->children_array_size>0){
	    ret_win = win_array_clicked(cur_win->children_array, cur_win->children_array_size, y, x);
	}
	if(ret_win)break;
	unsigned int clicked = wenclose(cur_win->nc_win, y, x);
	if(clicked == 1){
	    ret_win = cur_win;
	    break;
	}
    }
    return ret_win;
}

WIN* ctrl_win_return_clicked(CURR_SCREEN* curr_scr, int y, int x){
    //go through the win arrays that can contain cx and check if we click any of them
    WIN* ret_win = NULL;
    //go through the main win array that contains the generated cx contexts
    ret_win = win_array_clicked(curr_scr->win_array, curr_scr->win_array_size, y, x);
    //if we found that something is clicked stop searching further
    if(ret_win!=NULL)return ret_win;
    
    //go through the scroll buttons
    ret_win = win_array_clicked(curr_scr->scroll_bar_wins, 2, y, x);
    if(ret_win!=NULL)return ret_win;
    //go through the title_lr_win array only after scroll bars, since otherwise
    //ret_win != NULL when going through title_lr_win[1] (where scroll bars are)
    //and the loop through the scroll bars would not happen.
    //ALWAYS good practice to go through the smaller windows in the hierarchy first, then through the parents
    ret_win = win_array_clicked(curr_scr->title_lr_win, 2, y, x);
    if(ret_win)return ret_win;
    
    //otherwise go through the button win array
    ret_win = win_array_clicked(curr_scr->button_win_array, curr_scr->button_win_array_size, y, x);
    if(ret_win!=NULL)return ret_win;
    
    //look in the root array
    ret_win = win_array_clicked(curr_scr->root_win_array, curr_scr->root_win_array_size, y, x);
    if(ret_win!=NULL)return ret_win;
    
    return ret_win;
}

void ctrl_win_array_scroll(WIN* win_array[], WIN* win_parent, unsigned int* start, unsigned int size,
			   unsigned int list, unsigned int up){
    //if we want to scroll down
    if(up==0){
	int scroll = -1;
	scroll = win_layout_win_array(win_array, size, list, win_parent, *start);
	//if we dont need to scroll after all or if theres an error making the layout dont do anything else
	if(scroll<0)return;
	//add 1 to the last window that fits, so to not add it in the next page
	*start = scroll + 1;
	return;
    }
    else if(up==1){
	//check how many fit in one display, we do that with layout and a start of 0
	int total_scroll = win_layout_win_array(win_array, size, list, win_parent, 0);
	//if there is an error or somewhow now all the windows fit just make the start 0 again
	if(total_scroll<0){
	    *start = 0;
	    return;
	}
	//else calculate the new start, by subtracting the windows that currently fit from current start
	int new_start = *start - total_scroll - 1;
	//if new start is < 0 make start 0 again
	if(new_start<=0){
	    *start = 0;
	    return;
	}
	//else set the new start
	*start = new_start;
	return;
    }
}
