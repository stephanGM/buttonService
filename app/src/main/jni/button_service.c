/**
* ====================================================================
* button_service.c:
*   button service is the C portion of the service that handles the
*   input on gpios. It detects a change and determines the type of
*   press that has occured (single, double, long). It then calls
*   back to Java (ButtonService.java) and using the method
*   jniReturn(int action, int button) to broadcast this input so that
*   other applications may use the input as they please.
*
*   Currently the code is set to support 2 buttons gpios 2 and 21.
*   However, it can expanded to N number of buttons and this can be
*   configured by changing the globals below. Read the comments on
*   globals to know which ones to change.
*
* ====================================================================
* IMPORTANT NOTE:
*   This code assumes that gpio pins have been exported
*   and their edges set like so:
*
*   "echo XX  > /sys/class/gpio/export"
*   "echo in > /sys/class/gpio/gpioXX/direction"
*   "echo both > /sys/class/gpio/gpioXX/edge"
*
*   It also assumes that gpio pin values are initially 0
*   Depending on whether the pushbutton is a pullup or pulldown,
*   active_low can be set to make sure the initial state is 0:
*
*   "echo 1 > /sys/class/gpio/gpioXX/active_low"
*
*   If the app is given system priviledges it can configure the gpios
*   on its own using setup_gpios. This can be done by uncommenting
*   this function call below.
*   NOTE: currently will set up w active_low = 1
* ====================================================================
* author(s): Stephan Greto-McGrath
* ====================================================================
*/
#include <errno.h>
#include <jni.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <time.h>
#define BILLION 1000000000L
#define SHORT_PRESS 500000000
#define LONG_PRESS 1500000000
#define LOG_TAG "GPIO"
#ifndef EXEC
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#else
#define LOGD(...) printf(">==< %s >==< ",LOG_TAG),printf(__VA_ARGS__),printf("\n")
#endif

/* define the GPIO pins to be used */
static const int gpios[2] = {2,21};
static const int num_buttons = 2; /* num of buttons should correspond to num of gpios*/
/* gpio configuration functions */
void setup_gpios();
static int gpio_export(int pin);
static int set_edge(int pin, int edge);
static int set_active_low(int pin, int low);
/* helper functions */
int read_n_check(int i, int fd,JNIEnv *routineEnv, jobject obj, jmethodID mid);
void clock_start();
unsigned long long clock_end();
/* threads */
void *routine();
/* define a bool */
typedef enum {
    false,
    true
}bool;
/* indicates a press in not yet a double tap */
bool first_press[2]; /* size must match number of buttons */
/* both of these array are meant to hold 1 char either "1" or "0" */
/* [i][2] where i must match the number of GPIOS */
char buffers[2][2];
char prev_buffers[2][2];
/* for use in taking the difference in times */
struct timespec start;
struct timespec end;
/* cache jvm stuff to be used in thread */
static JavaVM *jvm;
static jclass cls;

/**
* ====================================================================
* startRoutine fn:
*   Called by Java to begin routine. Caches JVM to be used later in
*   the pthread that it spawns. FindClass() has trouble in new thread,
*   therefore class is also cached and made a global ref.
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
JNIEXPORT jint JNICALL
Java_com_google_hal_buttonservice_ButtonService_startRoutine(JNIEnv *env, jclass type) {
    LOGD("function begins");
    /* set all sentinel vars (first_press) to be true */
    int k;
    for (k=0; k< num_buttons; k++){
        first_press[k] = true;
    }
// TODO get system priviledges so it can set itself up
//    setup_gpios();

    /* cache JVM to attach native thread */
    int status = (*env)->GetJavaVM(env, &jvm);
    if (status != 0) {
        LOGD("failed to retrieve *env");
        exit(1);
    }
    pthread_t run;
    /* cls is made a global to be used in the spawned thread*/
    cls = (jclass)(*env)->NewGlobalRef(env,type);
    /* create the threads */
    pthread_create(&run, NULL, routine, NULL);
    /* not joined to UI thread (if it is, at high rates of input, there can be problems) */
}


/**
* ====================================================================
* routine fn:
*   Configures poll(), sets up infinite loop to run on new thread
*   in jvm and then uses poll() in read_n_check() to determine
*   if a change has taken place on a gpio pin. Based on this change,
*   the program will determine whether a short press, long press,
*   or double press has occured and call back to Java
*   (buttonService.java) in order to broadcast an intent allowing
*   other applications to act on this input.
* ====================================================================
* details:
*   CallVoidMethod() will be given 2 int args.
*   The first is the input code:
*   0: short(single) press     1:   long press      2: double press
*   The second is the button number in order to broadcast which button
*   has received the input.
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
 void *routine(){
    /* get a new environment and attach this new thread to jvm */
    JNIEnv* routineEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;
    args.name = NULL;
    args.group = NULL;
    (*jvm)->AttachCurrentThread(jvm,&routineEnv,&args);
    /* get method ID to call back to Java */
    jmethodID mid = (*routineEnv)->GetMethodID(routineEnv, cls, "jniReturn", "(II)V");
    jmethodID construct = (*routineEnv)->GetMethodID(routineEnv,cls,"<init>","()V");
    jobject obj = (*routineEnv)->NewObject(routineEnv, cls, construct);
    /* initialization of vars */
    struct pollfd pfd[num_buttons];
    int fds[num_buttons];
    char gpioValLocations[num_buttons][256];
    int i;
    /* set the locations to correspond to the chosen pins */
    for (i = 0; i < num_buttons ; i++){
        sprintf(gpioValLocations[i], "/sys/class/gpio/gpio%d/value", gpios[i]);
    }
    /* open file descriptors */
    for (i = 0; i < num_buttons; i++){
        if ((fds[i]= open(gpioValLocations[i],O_RDONLY)) < 0) {
            LOGD("failed on 1st open");
            exit(1);
        }
        pfd[i].fd = fds[i];  /* configure poll struct */
        pfd[i].events = POLLIN;
        lseek(fds[i], 0, SEEK_SET); /* consume any prior interrupts*/
        read(fds[i], buffers[i], sizeof buffers[i]);
    }
    unsigned long long diff;
    int breaker = 0;
    int new_val = 0;
    // TODO change POLLIN to POLLPRI if device has functional sysfs gpio interface (above and below)
    for (;;) {
        if (breaker == 1) break;
        poll(pfd, num_buttons, 1); // TODO if POLLPRI: change 3rd arg of poll() to -1
        for(i=0; i < num_buttons; i++){
            if ((pfd[i].revents & POLLIN)) {
                new_val = read_n_check(i, fds[i], routineEnv, obj, mid);
                if ((new_val == 1) && (atoi(buffers[i]) == 1)) { /* button is pressed */
                    clock_start();
                    first_press[i] = false;
                    while (first_press[i] == false) {
                        diff = clock_end();
                        new_val = read_n_check(i, fds[i], routineEnv, obj, mid);
                        if ((new_val == 0) && (atoi(buffers[i]) == 1) && (diff > LONG_PRESS)) {
                            /* button has not been lifted and "LONG_PRESS" time has elapsed */
                            /* call back to java with input code (1 = long) and button number (i) */
                            (*routineEnv)->CallVoidMethod(routineEnv, obj, mid, (jint)1, (jint) i);
                            first_press[i] = true; /* reset */
                        }
                        if ((new_val == 1) && (atoi(buffers[i]) == 0) && (diff < LONG_PRESS)) {
                            /* button has been lifted before "LONG_PRESS" time has elapse */
                            /* Start a timer. If time greater than "SHORT_PRESS" has elapsed
                             * without the button being pressed down again, we conclude it has been a single press,
                             * otherwise, if button is pressed again, it is a double press */
                            clock_start();
                            while (1) {
                                diff = clock_end();
                                new_val = read_n_check(i, fds[i], routineEnv, obj, mid);
                                if ((new_val == 1) && (atoi(buffers[i]) == 1) &&
                                    (diff < SHORT_PRESS)) {
                                    /* call back to java with input code (2 = double) and button number (i) */
                                    (*routineEnv)->CallVoidMethod(routineEnv, obj, mid, (jint)2, (jint) i);
                                    break;
                                }
                                if (diff > SHORT_PRESS) {
                                    /* call back to java with input code (0 = single) and button number (i) */
                                    (*routineEnv)->CallVoidMethod(routineEnv, obj, mid, (jint)0, (jint) i);
                                    break;
                                }
                            }
                            first_press[i] = true; /* reset */
                        }
                    }
                }else if (new_val == -1) { /* used to break free from infinite loop in case there are error reading gpios */
                    breaker = 1;
                }
            }
        }
    }
    /* shutdown */
    LOGD("Reading Terminated");
    for (i = 0; i < num_buttons; i++){
        close(fds[i]);
    }
    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
 }

/**
* ====================================================================
* read_n_check fn:
*   This function reads the gpio at the given fd into buffers[i]
*   and compares it to the previous value. If there is a change,
*   it reports it. It also calls Java to broadcast whether the button
*   is up or down.
* ====================================================================
* details:
*   CallVoidMethod() will be given 2 int args.
*   The first is the input code:
*   3: button is down     4: button is up
*   The second is the button number in order to broadcast which button
*   has received the input.
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
int read_n_check(int i, int fd,JNIEnv *routineEnv, jobject obj, jmethodID mid){
    /* copy current values to compare to next to detected change */
    memcpy(prev_buffers[i], buffers[i],2);
    /* read new values */
    if (lseek(fd, 0, SEEK_SET) == -1) return -1;
    if (read(fd, buffers[i], sizeof buffers[i]) == -1) return -1;
    if (atoi(prev_buffers[i]) != atoi(buffers[i])) { /* change is detected from last read value */
        if (atoi(buffers[i])==1){
            (*routineEnv)->CallVoidMethod(routineEnv, obj, mid, (jint)3, (jint) i); /* broadcast button down */
        }else if(atoi(buffers[i])==0){
            (*routineEnv)->CallVoidMethod(routineEnv, obj, mid, (jint)4, (jint) i); /* broadcast button up */
        }
        return 1;
    }
    return 0;
}

/**
 * ====================================================================
 * clock_start fn: log a start value to global start
 * ====================================================================
 * author(s): Stephan Greto-McGrath
 * ====================================================================
 */
void clock_start(){
    clock_gettime(CLOCK_MONOTONIC, &start);
}


/**
 * ====================================================================
 * clock_end fn: log an end value to global end
 *      returns: difference between start and end
 * ====================================================================
 * author(s): Stephan Greto-McGrath
 * ====================================================================
 */
unsigned long long clock_end(){
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (unsigned long long) BILLION * (end.tv_sec - start.tv_sec) +
           (end.tv_nsec - start.tv_nsec); /* return elapsed time */
}


/**
 * ====================================================================
 * setup_gpios fn: calls fns that export the desired gpios and sets
 *  their edges
 * ====================================================================
 * author(s): Stephan Greto-McGrath
 * ====================================================================
 */
void setup_gpios(){
    int k;
    for (k=0; k<num_buttons; k++){
        gpio_export(gpios[k]);
        set_edge(gpios[k],3);
        set_active_low(gpios[k],1);
    }
}

/**
 * ====================================================================
 * setup_gpios fn: exports gpio XX where int pin = XX
 * ====================================================================
 * author(s): Stephan Greto-McGrath
 * ====================================================================
 */
static int gpio_export(int pin){
    int fd;
    char buffer[3];
    fd = open("/sys/class/gpio/export", O_WRONLY);
    if(fd == -1){
        LOGD("Error! %s\n", strerror(errno));
        return(-1);
    }
    snprintf(buffer, 3, "%d", pin);
    if(write(fd, buffer, 3*sizeof(char))<0){
        LOGD("write during gpio export failed");
        return(-1);
    }
    close(fd);
    return(0);
}

/**
* ====================================================================
* set_edge fn: sets the edge of a given gpio pin
* ====================================================================
* Details:
*   sets edge as either rising (edge = 1), falling (edge = 2) or both
*   (edge = 3) on gpio of number pin
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
static int set_edge(int pin, int edge){
    int fd;
    char str[35];
    sprintf(str, "/sys/class/gpio/gpio%d/edge", pin);
    if ((fd = open(str, O_WRONLY)) <0){
        LOGD("open during set_edge failed");
        LOGD("Error! %s\n", strerror(errno));
        return(-1);
    }
    switch (edge) {
        case (1):
            if(write(fd, "rising", 6*sizeof(char))<0){
                LOGD("write during set_edge failed");
                return(-1);
            }
        case (2):
            if(write(fd, "falling", 7*sizeof(char))<0){
                LOGD("write during set_edge failed");
                return(-1);
            }
        case (3):
            if(write(fd, "both", 4*sizeof(char))<0){
                LOGD("write during set_edge failed");
                return(-1);
            }
        default:
            if(write(fd, "both", 4*sizeof(char))<0){
                LOGD("write during set_edge failed");
                return(-1);
            }
    }
    close(fd);
    return(0);
}


/**
* ====================================================================
* set_edge fn: sets the edge of a given gpio pin
* ====================================================================
* Details:
*   sets active_low as 1 or 0 (default is 1)
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
static int set_active_low(int pin, int low){
    int fd;
    char str[40];
    sprintf(str, "/sys/class/gpio/gpio%d/active_low", pin);
    if ((fd = open(str, O_WRONLY)) <0){
        LOGD("open during set_active_low failed");
        LOGD("Error! %s\n", strerror(errno));
        return(-1);
    }
    switch (low) {
        case (0):
            if(write(fd, "0", 2*sizeof(char))<0){
                LOGD("write during set_active_low failed");
                return(-1);
            }
        case (2):
            if(write(fd, "1", 2*sizeof(char))<0){
                LOGD("write during set_active_low failed");
                return(-1);
            }
        default:
            if(write(fd, "1", 2*sizeof(char))<0){
                LOGD("write during set_active_low failed");
                return(-1);
            }
    }
    close(fd);
    return(0);
}

