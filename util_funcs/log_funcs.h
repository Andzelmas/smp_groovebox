//function to append to the log file, should end with the \n line terminator
int log_append_logfile(const char* add_string, ...);

//function to get a line from the log file
char* log_getline_logfile(int line_num);

//calculate how many lines in the logfile
unsigned int log_calclines_logfile();

//function to clear the logfile so its empty
int log_clear_logfile();
