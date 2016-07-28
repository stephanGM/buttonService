/**
* ====================================================================
* rotary-encoder-service.c:
*   Used to receive interrupts on GPIO using poll() and handle
*   the interrupt with a finite state machine in order to
*   determine the direction of rotation of the encoder.
*   Each change of state triggers a call to fsm.
*   Therefore, to use detents, place desired fn call by fsm call.
* ====================================================================
* IMPORTANT NOTE:
*   This code assumes that gpio pins have been exported
*   and their edges set like so:
*
*   "echo XX  >/sys/class/gpio/export"
*   "echo in >/sys/class/gpio/gpioXX/direction"
*   "echo both >/sys/class/gpio/gpioXX/edge"
*
*   "echo YY  >/sys/class/gpio/export"
*   "echo in >/sys/class/gpio/gpioYY/direction"
*   "echo both >/sys/class/gpio/gpioYY/edge"
*
* ====================================================================
* author(s): Stephan Greto-McGrath (with a couple lines from SO)
* ====================================================================
*/
#include <errno.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#define LOG_TAG "GPIO"
#ifndef EXEC
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#else
#define LOGD(...) printf(">==< %s >==< ",LOG_TAG),printf(__VA_ARGS__),printf("\n")
#endif

/* define the GPIO pins to be used!!!! */
static const int gpio1 = 2;
static const int gpio2 = 21;
static const int numOfButtons = 2;
static const int gpios[2] = {2,21};


int fsm(int previousState, int currentState);
int concantenate(int x, int y);
void get_direction(char buf1[8], char buf2[8], JNIEnv *env, jobject obj,jmethodID mid);
void *routine();
void setup_gpios();
static int gpio_export(int pin);
static int set_edge(int pin, int edge);
typedef enum {
    false,
    true
}bool;

/**
 * stateA is initial state
 * stateB is a single press
 * stateC is a double-tap
 * stateD is a long single press
 */
enum {
    stateA = 00,
    stateB = 01,
    stateC = 11,
    stateD = 10
};
bool sentinel = true;   /* indicates 1st run -> get initial (previous) state */
                        /* if state is invalid sentinel = true i.e restart */
int currentState;
int previousState;
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

// TODO get system priviledges so it can set itself up
//    setup_gpios(gpio1, gpio2);

    /* cache JVM to attach native thread */
    int status = (*env)->GetJavaVM(env, &jvm);
    if (status != 0) {
        LOGD("failed to retrieve *env");
        exit(1);
    }
    /* cls is made a global to be used in the spawned thread*/
    cls = (jclass)(*env)->NewGlobalRef(env,type);

    pthread_t routine_thread; /*create routine thread */
    if (pthread_create( &routine_thread, NULL, routine, NULL)) {
        LOGD("Error creating thread");
        exit(1);
    }
}

/**
* ====================================================================
* routine fn:
*   Configures poll(), sets up infinite loop to run on new thread
*   in jvm and then uses poll() to determine if a change has
*   taken place on a gpio pin. If it has, it calls get_direction
*   to determine which way the encoder was turned.
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
 void *routine(){

    /* get a new environment and attach this new thread to jvm */
    JNIEnv* newEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&newEnv,&args);
    /* get method ID to call back to Java */
    jmethodID mid = (*newEnv)->GetMethodID(newEnv, cls, "jniReturn", "(I)V");
    jmethodID construct = (*newEnv)->GetMethodID(newEnv,cls,"<init>","()V");
    jobject obj = (*newEnv)->NewObject(newEnv, cls, construct);


    /* initialization of vars */
    struct pollfd pfd[numOfButtons];
    int fds[numOfButtons];
    const char gpioValLocations[numOfButtons][256];
    int i =0;
    for (i = 0; i < numOfButtons ; i += 1){
        sprintf(gpioValLocations[i], "/sys/class/gpio/gpio%d/value", gpios[i]);
    }
    char buffers[numOfButtons][8];

    /* open file descriptors */
    for (i = 0; i < numOfButtons; i+=1){
        if ((fds[i]= open(gpioValLocations[i],O_RDONLY)) < 0) {
            LOGD("failed on 1st open");
            exit(1);
        }
        pfd[i].fd = fds[i];  /* configure poll struct */
        pfd[i].events = POLLIN;
        lseek(fds[i], 0, SEEK_SET); /* consume any prior interrupts*/
        read(fds[i], buffers[i], sizeof buffers[i]);
    }

    //TODO change POLLIN to POLLPRI if device has functional sysfs gpio interface
    //TODO if POLLPRI change 3rd arg of poll() to -1
    for (;;) {
        /* wait for interrupt */
        poll(pfd, numOfButtons, 1);
        for(i=0; i <  numOfButtons; i+=1){
            if ((pfd[i].revents & POLLIN)) {
            /*interrupt received */
            /* consume interrupts & read values */
                if (lseek(fds[i], 0, SEEK_SET) == -1) goto houstonWeHaveAProblem; /* on little goto is not the end of the world, please remain calm */
                if (read(fds[i], buffers[i], sizeof buffers[i]) == -1) goto houstonWeHaveAProblem;
            }
            get_direction(buffers[0],buffers[1],newEnv,obj,mid);
        }
    }
    /* shutdown */
    houstonWeHaveAProblem:
    LOGD("Reading Terminated");
    for (i = 0; i < numOfButtons; i += 1){
        close(fds[i]);
    }
    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
 }

/**
* ====================================================================
* get_direction fn:
*   assigns state based on sentinel var and calls the fsm to determine
*   direction
* ====================================================================
* authors(s): Stephan Greto-McGrath
* ====================================================================
*/
void get_direction(char buf1[8], char buf2[8], JNIEnv *newEnv, jobject obj, jmethodID mid){
    if (sentinel != true) {  /* we already have a prev state */
        previousState = currentState;
        currentState = concantenate(atoi(buf1), atoi(buf2));
        /* In case poll returns constantly, this statement below
         * ensures that we only consider a change in state.
         * Unnecessary if poll() is working properly with interrupts
         */
        if (previousState != currentState) {
            int direction = fsm(currentState, previousState);
            LOGD("direction: %d\n", direction);
            (*newEnv)->CallVoidMethod(newEnv, obj, mid, (jint)direction); /* call back to java */
                                                                    /* act on state change */
        }
    } else { /* just starting -> need prev state */

        currentState = concantenate(atoi(buf1), atoi(buf2));
        sentinel = false;
    }
}

/**
* ====================================================================
* fsm fn:
*     Finite State Machine to decode the output of a
*     rotary encoder implemented as a pulldown switch
* ====================================================================
* Details:
*
* Rotary Encoder Output: cw 1, ccw 0, invalid -1
* How it works:
*     CW  ---->
*     CCW <----
*
*          +-----+     +-----+     +-----+
* Ch X     |     |     |     |     |     |
*       ---+     +-----+     +-----+     +-----
*
*             +-----+     +-----+     +-----+
* Ch Y        |     |     |     |     |     |
*       ------+     +-----+     +-----+     +-----
*
*       ^  ^  ^  ^  ^  ^  ^  ^  ^  ^  ^  ^  ^
* X    0| 1| 1| 0| 0| 1| 1| 0| 0| 1| 1| 0| 0|
* Y    0| 0| 1| 1| 0| 0| 1| 1| 0| 0| 1| 1| 0|
*
*     State: XY
*     ccw  prev.  cw    ccw  prev.  cw
*     01 <- 00 -> 10    B  <- A ->  D
*     11 <- 01 -> 00    C  <- B ->  A
*     10 <- 11 -> 01    D  <- C ->  B
*     00 <- 10 -> 11    A  <- D ->  C
*
*     All state transitions not defined are invalid
* ====================================================================
* author(s):  Stephan Greto-McGrath
* ====================================================================
*/
int fsm(int previousState, int currentState) {
    int direction;
    switch (previousState) {
        case stateA:
            if (currentState == stateD) {
                direction = 1; /* cw */
            } else if (currentState == stateB) {
                direction = 0; /* ccw */
            } else {
                direction = -1; /* not a valid state */
                sentinel = true; /* restart state machine */
            }
            break;
        case stateB:
            if (currentState == stateA) {
                direction = 1; /* cw */
            } else if (currentState == stateC) {
                direction = 0; /* ccw */
            } else {
                direction = -1; /* not a valid state */
                sentinel = true; /* restart state machine */
            }
            break;
        case stateC:
            if (currentState == stateB) {
                direction = 1; /* cw */
            } else if (currentState == stateD) {
                direction = 0; /* ccw */
            } else {
                direction = -1; /* not a valid state */
                sentinel = true; /* restart state machine */
            }
            break;
        case stateD:
            if (currentState == stateC) {
                direction = 1; /* cw */
            } else if (currentState == stateA) {
                direction = 0; /* ccw */
            } else {
                direction = -1; /* not a valid state */
                sentinel = true; /* restart state machine */
            }
            break;
        default:
            direction = -1; /* not a valid state */
            sentinel = true; /* restart state machine */
    }
    return direction;
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
    gpio_export(gpio1);
    gpio_export(gpio2);
    set_edge(gpio1,3);
    set_edge(gpio2,3);
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

/**
* ====================================================================
* concantenate fn: (int x, int y) -> int xy
* ====================================================================
* authors(s): Tavis Bohne (stackoverflow.com/a/12700533/3885972)
* ====================================================================
*/
int concantenate(int x, int y) {
    int pow = 10;
    while (y >= pow)
        pow *= 10;
    return x * pow + y;
}


