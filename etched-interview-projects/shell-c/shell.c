
#include <errno.h>
#include <ctype.h>
#include "format.h"
#include "shell.h"
#include "vector.h"
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <limits.h>  //chatGPT;I was having a problem with some limits being not recognized and I asked gpt what to do, which told me to include this.
#include "sstring.h"
#include <stddef.h>
#include <sys/param.h>
#include <signal.h>
#include <stdlib.h>

#define STAT_PATH_SIZE 256
#define BUFFER_SIZE 256
typedef struct process {
    char *command;
    pid_t pid;
} process;

void exec_for_and(char* c, char** splitted, char* and, char delim, vector* history, char* builtin[4], char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect);
void exec_for_or(char* c, char** splitted, char* or, char delim, vector* history, char* builtin[4], char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect);
void exec_for_end(char* c, char** splitted, char* end, char delim, vector* history, char* builtin[4], char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect);
int run_built_in(char** splitted, char* builtin[4], vector* history, char delim, char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect);
//void execute(); // this is intended for putting the execution blog inside. 
void interrupt_sig(int interrupt_signal){}

int handle_signal_command(char** splitted) {
    char* kill_ = "kill";
    char* stop = "stop";
    char* cont = "cont";

    int signal = 0;
    if (strcmp(splitted[0], kill_) == 0) {
        if (splitted[1] == NULL) {
            printf("No process found!\n");
            return 0;
        }
        signal = SIGKILL;
    }
    else if (strcmp(splitted[0], stop) == 0) {
        if (splitted[1] == NULL) {
            //printf("No process found!\n");
            return 0;
        }
        signal = SIGSTOP;
    }
    else if (strcmp(splitted[0], cont) == 0) {
        if (splitted[1] == NULL) {
            //printf("No process found!\n");
            return 0;
        }
        signal = SIGCONT;
    } else {
        //printf("Not a signal command!\n");
        return 0;
    }
    char* endptr;
    pid_t pid = strtol(splitted[1], &endptr, 10);
    if (*endptr != '\0') {
        print_invalid_command(splitted[0]);
        return 0;
    }
    if (kill(pid, signal) == 0) {
        if (signal == SIGKILL) {
            print_killed_process((int)pid, splitted[0]);
            return 1;
        }
        else if (signal == SIGSTOP) {
            print_stopped_process((int)pid, splitted[0]);
            return 1;
        }
        else if (signal == SIGCONT) {
            print_continued_process((int)pid, splitted[0]);
            return 1;
        }
    } else {
        print_no_process_found((int)pid);
        return 0;
    }
    return 0;
}
void get_command_only(char *input, char* redirect) {
    // Check for `>>` first
    char *redirect_pos = strstr(input, redirect);

    // If `>>` is not found, check for `>`
    if (redirect_pos == NULL) {
        return;
    }

    // If a redirection operator is found, truncate the command
    if (redirect_pos != NULL) {
        *redirect_pos = '\0';  // Null-terminate at the start of the redirection operator
    }
}

void free_process_inf(process_info* proc_inf) {
    if (proc_inf->start_str) {
        free(proc_inf->start_str);
    }
   
    if (proc_inf->time_str) {
        free(proc_inf->time_str);
    }
    if (proc_inf->command) {
        free(proc_inf->command);
    }
    free(proc_inf);
    proc_inf = NULL;
}
//int 
void cleanup() {
    int status;
    while(waitpid((pid_t)(-1), (&status), WNOHANG) > 0) {
        //printf("Catching zombies..\n");
        if (WIFEXITED(status)) {
            //printf("Exited!\n");
            //vector_erase(processes_archive, position);
        }
        //vector_erase(processes_archive, position);
    }
}
time_t get_btime() {
    time_t b_time = 0;
    FILE* file = fopen("/proc/stat", "r");
    if (!file) {
        perror("Error!");
        return -1;
    }
    char buffer[BUFFER_SIZE];
    while(fgets(buffer, BUFFER_SIZE, file)) {
        if (sscanf(buffer, "btime %ld", &b_time) == 1) {
            fclose(file);
            return b_time;
        }
    }
    return b_time;
}
void print_process_vector(vector* processes_archive) {
    printf("Vector size is %zu \n", vector_size(processes_archive));
    for (size_t i = 0; i < vector_size(processes_archive); ++i) {
        process* new_process = (process*) vector_get(processes_archive, i);
        printf("Command is %s and Pid is %d\n", new_process->command, new_process->pid);
    }
}

void trim_background(char *c) {
    size_t len = strlen(c);

    // Traverse backwards to find the first non-whitespace character before `&`
    while (len > 0 && isspace(c[len - 1])) {
        len--;
    }

    // If last character is '&', remove it along with any trailing spaces before it
    if (len > 0 && c[len - 1] == '&') {
        len--;  // Skip the `&` character
        // Remove preceding whitespace before `&`
        while (len > 0 && isspace(c[len - 1])) {
            len--;
        }
    }

    c[len] = '\0'; 
}
void run_ps(vector* processes_archive) {
    //print_process_vector(processes_archive);
    char path[STAT_PATH_SIZE];

    size_t number_of_processes = vector_size(processes_archive);
    print_process_info_header();
    for (size_t i = 0; i < number_of_processes; ++i) {
        cleanup();

        number_of_processes = vector_size(processes_archive);
        process* proc = (process*)vector_get(processes_archive, i);
        sprintf(path, "/proc/%d/stat", (int)proc->pid);
        //printf("Path is %s\n", path);
        FILE* file = fopen(path, "r");
        if (!file) {
            //printf("Error is %s\n", strerror(errno));
        } else {
            process_info* proc_inf = malloc(sizeof(process_info));
            proc_inf->command = calloc(256, sizeof(char));
            proc_inf->start_str = calloc(256, sizeof(char));
            proc_inf->time_str = calloc(256, sizeof(char));
            long long int time = 0;
            unsigned long int utime = 0;
            unsigned long int stime = 0;
            
            strncpy(proc_inf->command, proc->command, strlen(proc->command));
            //
            //printf("\nSize of the command is %zu\n", strlen(proc_inf->command));
            fflush(stdout);
            fscanf(file, "%d %*s %c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu %*ld %*ld %*ld %*ld %ld %*ld %llu %lu"
                ,&proc_inf->pid, &proc_inf->state, &utime, &stime, &proc_inf->nthreads, &time, &proc_inf->vsize);
            proc_inf->vsize /= 1024;
            //printf("Proc_inf vsize is %lu\n", proc_inf->vsize);
            time_t time_ = (time / sysconf(_SC_CLK_TCK)) + get_btime();
            struct tm *time_info = localtime(&time_);
            time_struct_to_string(proc_inf->start_str, sizeof(proc_inf->start_str), time_info);
            time_t time_two = (utime + stime)/sysconf(_SC_CLK_TCK);
            execution_time_to_string(proc_inf->time_str, sizeof(proc_inf->time_str), (time_two - time_two%60)/60, time_two%60);
            print_process_info(proc_inf);
            fclose(file);
            //print_process_vector(processes_archive);
            free_process_inf(proc_inf);
        }
    }

}

// These BIG 3 are created for saving processes inside a vector.
void* process_copy_constructor(void *elem) {
    if (elem == NULL) {
        return NULL;
    }
    process* elem_process = (process*)elem;
    process* copy_process = malloc(sizeof(process));
    copy_process->pid = elem_process->pid;
    if (elem_process->command) {
        copy_process->command = strdup(elem_process->command);
    } else {
        copy_process->command = NULL;
    }
    return copy_process;
}
void process_destructor(void* elem) {
    if (elem == NULL) {
        return;
    }
    process* elem_process = (process*)elem;
    elem_process->pid = 0;
    if (elem_process->command) {
        free(elem_process->command);
    }
    free(elem_process);
}
void* process_default_constructor() {
    process* process = malloc(sizeof(process));
    if (process == NULL) {
        return NULL;
    }
    process->pid = 100;
    process->command = "echo";
    return process;
}

void create_struct_and_push(vector* processes_archive, char* command, int pid) {
   // printf("Command is %s and its pid is %d\n", command, pid);
    process* new_process = malloc(sizeof(process));
    new_process->pid = pid;
    new_process->command = command;
    vector_push_back(processes_archive, process_copy_constructor(new_process));
}

int if_background_command(char *command, ssize_t input) {
    if (command[input - 2] == '&') {
        return 1;
    }
    return 0;
}

char* find_exec_logic(char* string) {
    char* and = "&&";
    char* or = "||";
    char* end = ";";
    char* string_iter = string;
    while(*string_iter) {
        if (strncmp(string_iter, and, strlen(and)) == 0) {
            return and;
        }
        else if (strncmp(string_iter, or, strlen(or)) == 0) {
            return or;
        }
        else if (strncmp(string_iter, end, strlen(end)) == 0) {
            return end;
        }
        string_iter++;
    }
    return NULL;
}
char *find_redirection(char* string, char* builtin[3]) {
    char* string_iter = string;
    char* false_flag = "<<";
    while(*string_iter) {
        if (strncmp(string_iter, builtin[1], strlen(builtin[1])) == 0) {
            return builtin[1];
        }
        else if (strncmp(string_iter, builtin[1], strlen(builtin[0])) == 0) {
            return builtin[0];
        }
        else if (strncmp(string_iter, false_flag, strlen(false_flag)) == 0) {
            string_iter++;
        }
        else if (strncmp(string_iter, builtin[2], strlen(builtin[2])) == 0) {
            return builtin[2];
        }
        string_iter++;
    }
    return NULL;
}
void write_history(vector* history, FILE* file) {
    for (size_t i = 0; i < vector_size(history); ++i) {
        char* str = (char*) vector_get(history, i);
        fwrite(str, sizeof(char), strlen(str), file);
    }
}

char* history_search(char* query, vector* vector) {
    if (strncmp(query, "!", strlen("!")) == 0) {
        char* str = (char*)vector_get(vector, vector_size(vector) - 1);
        return str;
    }
    for(size_t i = vector_size(vector); i > 0 ; i--) {
        char* str = (char*)vector_get(vector, i - 1);
        if (strlen(query) > strlen(str)) {
            continue;
        } else {
            if (strncmp(query, str, strlen(query)) == 0) {
                return str;
            }
        }
    }
    print_no_history_match();
    return NULL;
}
char *concat(char** splitted) {
    int i = 0;
    int split_length = 0;
    while(splitted[i]) {
        split_length++;
        i++;
    }
    if (split_length > 1) {
        int j = 1;
        sstring* present = NULL;
        if (isalpha(splitted[0][0])) {
            present = cstr_to_sstring(splitted[0]);
        } else {
            present = cstr_to_sstring(splitted[0] + 1);
        }
        while(j < split_length) {
            if (j <= split_length - 1) {
                sstring_append(present, cstr_to_sstring(" "));
            }

            sstring_append(present, cstr_to_sstring(splitted[j]));
            j++;
        }
        return sstring_to_cstr(present);
    } else {
        if (isalpha(splitted[0][0])) {
            return splitted[0];
        } else {
            return splitted[0] + 1;
        }
    }
    return NULL;

}
void file_write(vector* history, char* builtin[4], FILE* write) {
    for (size_t i = 0; i < vector_size(history); ++i) {
        char* string = (char*) vector_get(history, i);
        if (strcmp(string, builtin[1]) == 0 || strncmp(string, builtin[2], strlen(builtin[2])) == 0 || strncmp(string, builtin[3], strlen(builtin[3])) == 0) {
            continue;
        } else {
            fwrite(string, sizeof(char), strlen(string), write);
            fwrite("\n", sizeof(char), strlen("\n"), write);
            fflush(write);
        }
    }
}
void selective_push(char* string, vector* history, char *builtin[4]) {

    if (strcmp(string, builtin[1]) == 0 || strncmp(string, builtin[2], strlen(builtin[2])) == 0 || strncmp(string, builtin[3], strlen(builtin[3])) == 0) {
        return;
    } else {
        vector_push_back(history, strdup(string));
    }
}
// I asked chatGPT to help me with my trailng whitespace and it helped me figure out my issue.
void tailing_whitespace(char* string) {
    if (string == NULL) {
        return;
    }
    char* start = string;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != string) {
        memmove(string, start, strlen(start) + 1);
    }
}

void trailing_spaces(char** splitted) {
    int i = 0;
    while (splitted[i]) {
        size_t len = strlen(splitted[i]);
        while (len > 0 && (splitted[i][len - 1] == ' ' || splitted[i][len - 1] == '\n')) {
            splitted[i][len - 1] = '\0';
            len--;
        }
        i++;
    }
}
char** vector_print(vector* vector) {
    size_t vector_length = vector_size(vector);
    char** array = malloc(sizeof(char*) * (vector_length + 1));
    for (size_t i = 0; i < vector_length; ++i) {
        char* str = (char*)vector_get(vector, i);
        array[i] = malloc(strlen(str) + 1);
        strcpy(array[i], str);
    }
    array[vector_length] = NULL;
    return array;
}

int if_built_in(char* command, char* builtin[4]) {
    for(int i = 0; i < 4; ++i) {
        if (strncmp(command, builtin[i], strlen(builtin[i])) == 0)  {
            return 1;
        }
    }
    return 0;
}
int fork_exec_wait(char** splitted, int is_background, vector* processes_archive, char* filename, int is_redirect, char* redirect) {
    trailing_spaces(splitted);
    char* output = ">";
    char* append = ">>";
    char* input = "<";
    pid_t pid = fork();
    if (pid < 0) {
        print_fork_failed();
        exit(0);
    } 
    else if (pid > 0) {
        int status;
        //printf("Concatted argument is %s\n", concat(splitted));
        create_struct_and_push(processes_archive, concat(splitted), pid);
        if (is_background == 1) {
            waitpid(pid, &status, WNOHANG);
        } else {
            waitpid(pid, &status, 0);
        }
        if (WIFEXITED(status)) {
            fflush(stdout);
            return WEXITSTATUS(status);
        } else {
            return 1;
        }
    } else {
        //fflush(stdout);
        int file_descp = 0;
        if (is_background == 1) {
            setpgid(0, 0);
        }
        print_command_executed(getpid());
        if (is_redirect == 1) {
            mode_t mode = S_IRUSR | S_IWUSR;
            if (strcmp(redirect, output) == 0) {
                file_descp = open(filename, O_TRUNC | O_RDWR, mode);
                if (dup2(file_descp, STDOUT_FILENO) < 0) {
                    print_redirection_file_error();
                    close(file_descp);
                    exit(0);
                }
            } else if (strcmp(redirect, append) == 0) {
                file_descp = open(filename, O_CREAT| O_APPEND | O_RDWR, mode);
                if (dup2(file_descp, STDOUT_FILENO) < 0) {
                    print_redirection_file_error();
                    close(file_descp);
                    exit(0);
                }
            } else if (strcmp(redirect, input) == 0) {
                file_descp = open(filename, O_RDONLY, mode);
                if (dup2(file_descp, STDIN_FILENO) < 0) {
                    print_redirection_file_error();
                    close(file_descp);
                    exit(0);
                }
            }
            close(file_descp);
        }
        execvp(splitted[0], splitted);
        fflush(stdout);
        print_exec_failed(splitted[0]);
        exit(0);
    }
    return 1;
}
int run_built_in(char** splitted, char* builtin[4], vector* history, char delim, char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect) {
    trailing_spaces(splitted);
    if (strcmp(splitted[0], builtin[0]) == 0) { // this is for cd
        char* full_path = get_full_path(splitted[1]);
        if (splitted[1] == NULL) {
            print_no_directory(splitted[1]);
            return 1;
        }
        if (full_path == NULL || chdir(full_path) < 0) { 
            print_redirection_file_error();
            return 1;
        } else {
            return 0;
        }
    }
    else if (strcmp(splitted[0], builtin[1]) == 0) { //this for !history
        if (splitted[0] == NULL) {
            print_no_directory(splitted[0]);
            return 1;
        } else {
            for (size_t i = 0; i < vector_size(history); ++i) {
                printf("%zu\t%s\n", i, (char*)vector_get(history, i));
            }
            return 1;
        }
    }
    else if (splitted[0][0] == '#') { //this for #<n>
        if (isdigit(splitted[0][1])) {
            int index = atoi(splitted[0] + 1);
            if (index >= 0 && index < (int)vector_size(history)) {
                char* command_in_hist = (char*)vector_get(history, atoi(splitted[0] + 1));
                //selective_push(command_in_hist, history, builtin);
                char* find_logic = find_exec_logic(strdup(command_in_hist));
                print_command(command_in_hist);
                if (find_logic == NULL) {
                    vector* split = sstring_split(cstr_to_sstring(strdup(command_in_hist)), delim);
                    splitted = vector_print(split);
                    selective_push(command_in_hist, history, builtin);
                    if (if_built_in(splitted[0], builtin) == 1) {
                        int last_status = run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
                        return last_status;
                    }
                    else {
                        int last_status = fork_exec_wait(splitted, 0, processes_archive, filename, is_redirect, redirect);
                        return last_status;
                    }
                } else if (strncmp(find_logic, punct[0], strlen(punct[0])) == 0) {
                    exec_for_and(command_in_hist, splitted, punct[0], delim, history, builtin, punct, processes_archive, filename, is_redirect, redirect);

                } else if (strncmp(find_logic, punct[1], strlen(punct[1])) == 0) {
                    exec_for_or(command_in_hist, splitted, punct[1], delim, history, builtin, punct, processes_archive, filename, is_redirect, redirect);

           
                }  else if (strncmp(find_logic, punct[2], strlen(punct[2])) == 0) {
                    exec_for_end(command_in_hist, splitted, punct[2], delim, history, builtin, punct, processes_archive, filename, is_redirect, redirect);
                }
            }
            else {
                print_invalid_index();
                return 1;
            }
        } else {
            print_invalid_command(splitted[0]);
            return 1;
        }
    }
    else if (splitted[0][0] == '!') { //for !prefix
        if (isalpha(splitted[0][1]) || strncmp(splitted[0], "!", strlen("!")) == 0) {
            char* input = concat(splitted);
            char* search = history_search(input, history);
            if (search == NULL) {
                return 1;
            }
            char* find_logic = find_exec_logic(strdup(search));
            print_command(search);
            if (find_logic == NULL) {
                    vector* split = sstring_split(cstr_to_sstring(strdup(search)), delim);
                    splitted = vector_print(split);
                    selective_push(search, history, builtin);
                    if (if_built_in(splitted[0], builtin) == 1) {
                        int last_status = run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
                        return last_status;
                    }
                    else {
                        int last_status = fork_exec_wait(splitted, 0, processes_archive, filename, is_redirect, redirect);
                        return last_status;
                    }
                } else if (strncmp(find_logic, punct[0], strlen(punct[0])) == 0) {
                    exec_for_and(search, splitted, punct[0], delim, history, builtin, punct, processes_archive, filename, is_redirect, redirect);

                } else if (strncmp(find_logic, punct[1], strlen(punct[1])) == 0) {
                    exec_for_or(search, splitted, punct[1], delim, history, builtin, punct, processes_archive, filename, is_redirect, redirect);

           
                }  else if (strncmp(find_logic, punct[2], strlen(punct[2])) == 0) {
                    exec_for_end(search, splitted, punct[2], delim, history, builtin, punct, processes_archive, filename, is_redirect, redirect);
                }
        } 
        else {
            return 1;
        }
    }
    return 1;
}

void see_vector(vector* vector) {
    for (size_t i = 0; i < vector_size(vector); ++i) {
        printf("%zu:  %s \n", i, (char*)vector_get(vector, i));
    }
}
void destroy(char** string, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        free(string[i]);
    }
    free(string);
}
void exec_for_or(char* c, char** splitted, char* or, char delim, vector* history, char* builtin[4], char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect) {
    if (c[0] == '!') {
        vector* split = sstring_split(cstr_to_sstring(strdup(c)), delim);
        splitted = vector_print(split);
        trailing_spaces(splitted);
        run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
        return;
    }
    int last_status = 100;
    char* c_split = strtok(strdup(c), or);
    selective_push(c, history, builtin);
    do {
        tailing_whitespace(c_split);
        if (last_status != 100) {
            if (last_status == 0) {
                return;
            } else {
                vector* split = sstring_split(cstr_to_sstring(c_split), delim);
                splitted = vector_print(split);
                trailing_spaces(splitted);
                //selective_push(c_split, history, builtin);
                if (if_built_in(splitted[0], builtin) == 1) {
                    last_status = run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
                }
                else {
                    last_status = fork_exec_wait(splitted, 0, processes_archive, filename, is_redirect, redirect);
                }
            }
        } else {
            vector* split = sstring_split(cstr_to_sstring(c_split), delim);
            splitted = vector_print(split);
            trailing_spaces(splitted);
            //selective_push(c_split, history, builtin);
            if (if_built_in(splitted[0], builtin) == 1) {
                last_status = run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
            }
            else {
                last_status = fork_exec_wait(splitted, 0, processes_archive, filename, is_redirect, redirect);
            }
        }
    } while((c_split = strtok(NULL, or)));
}
void exec_for_and(char* c, char** splitted, char* and, char delim, vector* history, char* builtin[4], char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect) {
    if (c[0] == '!') {
        vector* split = sstring_split(cstr_to_sstring(strdup(c)), delim);
        splitted = vector_print(split);
        trailing_spaces(splitted);
        run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename,  is_redirect, redirect);
        return;
    }
    int last_status = 100;
    char* c_split = strtok(strdup(c), and);
    selective_push(c, history, builtin);
    do {
       tailing_whitespace(c_split);
       if (last_status != 100) {
            if (last_status == 0) {
                vector* split = sstring_split(cstr_to_sstring(c_split), delim);
                splitted = vector_print(split);
                trailing_spaces(splitted);
                //selective_push(c_split, history, builtin);
                if (if_built_in(splitted[0], builtin) == 1) {
                    last_status = run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
                }
                else {
                    last_status = fork_exec_wait(splitted, 0, processes_archive, filename, is_redirect, redirect);
                }
            } else {
                return;
            } 
        } else {
            vector* split = sstring_split(cstr_to_sstring(strdup(c_split)), delim);
            splitted = vector_print(split);
            trailing_spaces(splitted);
            //selective_push(c_split, history, builtin);
            if (if_built_in(splitted[0], builtin) == 1) {
                last_status = run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
            }
            else {
                last_status = fork_exec_wait(splitted, 0, processes_archive, filename,  is_redirect, redirect);
           }
        }
    } while((c_split = strtok(NULL, and)));
}
void exec_for_end(char* c, char** splitted, char* end, char delim, vector* history, char* builtin[4], char* punct[3], vector* processes_archive, char* filename, int is_redirect, char* redirect) {
    //int last_status = 0;
    if (c[0] == '!') {
        vector* split = sstring_split(cstr_to_sstring(strdup(c)), delim);
        splitted = vector_print(split);
        trailing_spaces(splitted);
        run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, redirect);
        return;
    }
    char* c_split = strtok(strdup(c), end);
    selective_push(c, history, builtin);
    do {
        tailing_whitespace(c_split);
        vector* split = sstring_split(cstr_to_sstring(strdup(c_split)), delim);
        splitted = vector_print(split);
        trailing_spaces(splitted);
        //selective_push(c_split, history, builtin);
        if (if_built_in(splitted[0], builtin) == 1) {
            run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename,  is_redirect, redirect);
        }
        else {
            fork_exec_wait(splitted, 0, processes_archive, filename, is_redirect, redirect);
        }
    } while((c_split = strtok(NULL, end)));
}
int shell(int argc, char *argv[]) {
    // TODO: Code until !prefix and then reread the MP file!
    //History mode -h writing
    vector* processes_archive = vector_create(process_copy_constructor, process_destructor, process_default_constructor);
    process* shell_proc = malloc(sizeof(process));
    shell_proc->pid = getpid();
    shell_proc->command = "./shell";
    vector_push_back(processes_archive, process_copy_constructor(shell_proc));
    //char* filename = NULL;
    int is_background = 0;
    int is_redirect = 0;
    char* punct[3] = {"&&", "||", ";"};
    char* direction[3] = {">", ">>", "<"};
    char* and = "&&";
    char* or = "||";
    char* end = ";";
    char* ps = "ps";
    char *builtin[4] = {"cd", "!history", "#","!"};
    char delim = ' ';
    char* exit_ = "exit";
    char* mode = NULL;
    vector* history = string_vector_create();
    setbuf(stdout, NULL);
    char branch[PATH_MAX];
    char* c = NULL;
    size_t capacity = 0;
    int opt; 
    FILE* read = NULL;
    FILE* write = NULL;
    FILE* close_ = NULL;
    char* filename = NULL;
    while ((opt = getopt(argc, argv, "h:f:")) != -1) {
        switch(opt) {
            case 'h':
                mode = "a+";
                filename = optarg;
                read = stdin;
                write = fopen(get_full_path(filename), mode);
                if (!write) {
                    print_history_file_error();
                    break;
                }
                close_ = write;
                break;
            case 'f':
                mode = "r";
                filename = optarg;
                read = fopen(get_full_path(filename), mode);
                if (!read) {
                    print_script_file_error();
                    break;
                }
                write = stdout;
                close_ = read;
                break;
            default: //important to check
                break;
        }
        signal(SIGINT, interrupt_sig);
        while(1) {
            is_background = 0;
            is_redirect = 0;
            const char* directory = getcwd(branch, sizeof(branch));
            print_prompt(directory, getpid());

            ssize_t input = getline(&c, &capacity, read);

            if (input == -1 || strncmp(c, exit_, strlen(exit_)) == 0) {
                if (write != stdout) {
                    file_write(history, builtin, write);
                }
                //print_process_vector(processes_archive);
                vector_destroy(history);
                vector_destroy(processes_archive);
                exit(0);
            }

            //file_write(c, builtin, write);

            if (if_background_command(c, input) == 1) {
                trim_background(c);
                is_background = 1;
            }

            if (c[input - 1] == '\n') {
                c[input- 1] = '\0';
            }

            if (strcmp(c, ps) == 0) {
                run_ps(processes_archive);
            } else {
                char* find_redirect = find_redirection(strdup(c), direction);
                if (find_redirect != NULL) {
                    is_redirect = 1;
                    if (strcmp(find_redirect, direction[0]) == 0) {
                        filename = strstr(strdup(c), direction[0]) + 2;
                        get_command_only(c, find_redirect);
                        c[strlen(c) - 1] = '\0';
                    }
                    else if (strcmp(find_redirect, direction[1]) == 0) {
                        filename = strstr(strdup(c), direction[1]) + 3;
                        get_command_only(c, find_redirect);
                        c[strlen(c) - 1] = '\0';
                    }
                    else if (strcmp(find_redirect, direction[2]) == 0) {
                        filename = strstr(strdup(c), direction[2]) + 2;
                        get_command_only(c, find_redirect);
                        c[strlen(c) - 1] = '\0';
                    }
                }
                char* find_logic = find_exec_logic(strdup(c));
                char** splitted = NULL;

    
                if (find_logic == NULL) {
                    vector* split = sstring_split(cstr_to_sstring(strdup(c)), delim);
                    splitted = vector_print(split);
                    //print_command_executed(getpid());
                    int result_ = handle_signal_command(splitted);
                    selective_push(c, history, builtin);
                    if (result_ != 1) {
                        if (if_built_in(splitted[0], builtin) == 1) {
                            run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect,find_redirect);
                        }
                        else {
                            fork_exec_wait(splitted, is_background, processes_archive, filename, is_redirect,find_redirect);
                        }
                    }
                } 
                else if (strncmp(find_logic, and, strlen(and)) == 0) {
                    exec_for_and(c, splitted, and, delim, history, builtin, punct, processes_archive, filename, is_redirect,find_redirect);
            
                } else if (strncmp(find_logic, or, strlen(or)) == 0) {
                    exec_for_or(c, splitted, or, delim, history, builtin, punct, processes_archive, filename, is_redirect,find_redirect);
    
            
                } else if (strncmp(find_logic, end, strlen(end)) == 0) {
                    exec_for_end(c, splitted, end, delim, history, builtin, punct, processes_archive, filename, is_redirect,find_redirect);
                }
            }
            //cleanup();
        }
        fclose(close_);
    }
    signal(SIGINT, interrupt_sig);
    while(1) {
        is_background = 0;
        is_redirect = 0;
        //int last_status = 1000;
        const char* directory = getcwd(branch, sizeof(branch));
        print_prompt(directory, getpid());
        
        ssize_t input = getline(&c, &capacity, stdin);
        
       


        if (input == -1 || strncmp(c, exit_, strlen(exit_)) == 0) {
            //print_process_vector(processes_archive);
            vector_destroy(history);
            vector_destroy(processes_archive);
            exit(0);
        }
         
        if (if_background_command(c, input) == 1) {
            //printf("Found the background! \n");
            trim_background(c);
            is_background = 1;
        }

        //file_write(c, builtin, stdout);
        

        if (c[input - 1] == '\n') {
            c[input- 1] = '\0';
        }

        if (strcmp(c, ps) == 0) {
            run_ps(processes_archive);
        } else {

            char* find_redirect = find_redirection(strdup(c), direction);
            if (find_redirect != NULL) {
                is_redirect = 1;
                if (strcmp(find_redirect, direction[0]) == 0) {
                    filename = strstr(strdup(c), direction[0]) + 2;
                    get_command_only(c, find_redirect);
                    c[strlen(c) - 1] = '\0';
                    
                }
                else if (strcmp(find_redirect, direction[1]) == 0) {
                    filename = strstr(strdup(c), direction[1]) + 3;
                    get_command_only(c, find_redirect);
                    c[strlen(c) - 1] = '\0';
                }
                else if (strcmp(find_redirect, direction[2]) == 0) {
                    filename = strstr(strdup(c), direction[2]) + 2;
                    get_command_only(c, find_redirect);
                    c[strlen(c) - 1] = '\0';
                }
            }
            char* find_logic = find_exec_logic(strdup(c));
            char** splitted = NULL;

            if (find_logic == NULL) {
                vector* split = sstring_split(cstr_to_sstring(strdup(c)), delim);
                splitted = vector_print(split);
                int return_ = handle_signal_command(splitted);
                selective_push(c, history, builtin);
                if (return_ != 1) {
                    if (if_built_in(splitted[0], builtin) == 1) {
                        run_built_in(splitted, builtin, history, delim, punct, processes_archive, filename, is_redirect, find_redirect);
                    }
                    else {
                        fork_exec_wait(splitted, is_background, processes_archive, filename, is_redirect, find_redirect);
                    } 
                }
            } else if (strncmp(find_logic, and, strlen(and)) == 0) {
                exec_for_and(c, splitted, and, delim, history, builtin, punct, processes_archive, filename, is_redirect, find_redirect);

            } else if (strncmp(find_logic, or, strlen(or)) == 0) {
                exec_for_or(c, splitted, or, delim, history, builtin, punct, processes_archive, filename, is_redirect,find_redirect);

           
            } else if (strncmp(find_logic, end, strlen(end)) == 0) {
                exec_for_end(c, splitted, end, delim, history, builtin, punct, processes_archive, filename, is_redirect,find_redirect);
            }
        }
       
        //cleanup();

    }
    fclose(stdout);
    //print_process_vector(processes_archive);
    return 0;

    //Updates:
    //INPUT redirection. Problem: The way I read the stdin causes a problem. 
    //YB 4
}