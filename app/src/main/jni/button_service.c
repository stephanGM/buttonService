//TODO make sure first_press[i] and button_down[i] are thread safe

/**
* ====================================================================
* button_service.c:
*
* ====================================================================
* IMPORTANT NOTE:
*   This code assumes that gpio pins have been exported
*   and their edges set like so:
*
*   "echo XX  >/sys/class/gpio/export"
*   "echo in >/sys/class/gpio/gpioXX/direction"
*   "echo both >/sys/class/gpio/gpioXX/edge"
*
*   It also assumes that gpio pin values are initially 0
*
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

/* define the GPIO pins to be used!!!! */
static const int gpios[2] = {2,21};
static const int num_buttons = 2; /* num of buttons should correspond to num of gpios*/
/* gpio configuration functions */
void setup_gpios();
static int gpio_export(int pin);
static int set_edge(int pin, int edge);
/* helper functions */
int read_n_check(int i, int fd);
void clock_start();
unsigned long long clock_check();
/* threads */
void *routine();
/* define a bool */
typedef enum {
    false,
    true
}bool;
/* indicates a press in not yet a double tap */
bool first_press[2]; /* size must match number of buttons */
bool button_down[2];
/* both of these array are meant to hold 1 char either "1" or "0" */
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
*   the pthreads that it spawns. FindClass() has trouble in new thread,
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
//    setup_gpios(gpio1, gpio2);

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
//    TODO figure out why joining threads causes crash when input lvls are high
}


/**
* ====================================================================
* routine fn:
*   Configures poll(), sets up infinite loop to run on new thread
*   in jvm and then uses poll() to determine if a change has
*   taken place on a gpio pin.
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
    jmethodID mid = (*routineEnv)->GetMethodID(routineEnv, cls, "jniReturn", "(I)V");
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
    //TODO change POLLIN to POLLPRI if device has functional sysfs gpio interface
    //TODO if POLLPRI: change 3rd arg of poll() to -1
    for (;;) {
        if (breaker == 1) break;
        poll(pfd, num_buttons, 1);
        for(i=0; i < num_buttons; i++){
            if ((pfd[i].revents & POLLIN)) {
                new_val = read_n_check(i, fds[i]);
                if ((new_val == 1) && (atoi(buffers[i]) == 1)) { /* button is pressed */
                    LOGD("change detected");
                    clock_start();
                    first_press[i] = false;
                    while (first_press[i] == false) {
                        diff = clock_check();
                        new_val = read_n_check(i, fds[i]);
                        // todo make sure all up and down presses are broadcast
                        if ((new_val == 0) && (atoi(buffers[i]) == 1) && (diff > LONG_PRESS)) {
                            // button has not been lifted
                            // button is still down
                            // long press time has elapsed
                            // broadcast long press
                            LOGD("long");
                            // broadcast
                            first_press[i] = true; /* reset */
                        }
                        if ((new_val == 1) && (atoi(buffers[i]) == 0) && (diff < LONG_PRESS)) {
                            // button has been raised before longpress
                            // start timer
                            clock_start();
                            while (1) {
                                diff = clock_check();
                                if ((read_n_check(i, fds[i]) == 1) && (atoi(buffers[i]) == 1) &&
                                    (diff < SHORT_PRESS)) {
                                    // ouput double tap
                                    LOGD("double");
                                    break;
                                }
                                if (diff > SHORT_PRESS) {
                                    // output single-press
                                    LOGD("short");
                                    break;
                                }

                            }
                            first_press[i] = true; /* reset */
                        }
                    }

                }else if (new_val == -1) {
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

int read_n_check(int i, int fd){
    /* copy current values to compare to next to detected change */
    memcpy(prev_buffers[i], buffers[i],2);
    /* read new values */
    if (lseek(fd, 0, SEEK_SET) == -1) return -1;
    if (read(fd, buffers[i], sizeof buffers[i]) == -1) return -1;
    if (atoi(prev_buffers[i]) != atoi(buffers[i])) { /* change is detected from last read value */
        return 1;
    }
    return 0;
}

void clock_start(){
    clock_gettime(CLOCK_MONOTONIC, &start);
}

unsigned long long clock_check(){
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (unsigned long long) BILLION * (end.tv_sec - start.tv_sec) +
           (end.tv_nsec - start.tv_nsec); /* check elapsed time */
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
                LOGD("write during gpio export failed");
                return(-1);
            }
        case (2):
            if(write(fd, "falling", 7*sizeof(char))<0){
                LOGD("write during gpio export failed");
                return(-1);
            }
        case (3):
            if(write(fd, "both", 4*sizeof(char))<0){
                LOGD("write during gpio export failed");
                return(-1);
            }
        default:
            if(write(fd, "both", 4*sizeof(char))<0){
                LOGD("write during gpio export failed");
                return(-1);
            }
    }
    close(fd);
    return(0);
}


