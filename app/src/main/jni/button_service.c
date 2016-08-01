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
#define LOG_TAG "GPIO"
#ifndef EXEC
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#else
#define LOGD(...) printf(">==< %s >==< ",LOG_TAG),printf(__VA_ARGS__),printf("\n")
#endif

/* define the GPIO pins to be used!!!! */
static const int gpios[2] = {2,21};
static const int num_buttons = 2; /* num of buttons should correspond to num of gpios*/
/* create a thread array */
pthread_t threads[4];
/* gpio configuration functions */
void setup_gpios();
static int gpio_export(int pin);
static int set_edge(int pin, int edge);
/* threads */
void *master();
void *routine();
void *single_press();
void *long_press();
/* define a bool */
typedef enum {
    false,
    true
}bool;

bool first_press = true; /* indicates a press in not yet a double tap */

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
    /* create the threads */
    pthread_create(&threads[0], NULL, master, NULL);
    pthread_create(&threads[1], NULL, routine, NULL);
//    TODO figure out why joining threads causes crash when input lvls are high
//    int k;
//    for (k=0; k<num_threads; k++){
//        pthread_join(threads[k],NULL);
//    }
}

void *master(){
    /* get a new environment and attach this new thread to jvm */
    JNIEnv* masterEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&masterEnv,&args);
//    for(;;){
//        LOGD("master is running");
//        sleep(1);
//    }
    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
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
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&routineEnv,&args);
    /* get method ID to call back to Java */
    jmethodID mid = (*routineEnv)->GetMethodID(routineEnv, cls, "jniReturn", "(I)V");
    jmethodID construct = (*routineEnv)->GetMethodID(routineEnv,cls,"<init>","()V");
    jobject obj = (*routineEnv)->NewObject(routineEnv, cls, construct);
    /* initialization of vars */
    struct pollfd pfd[num_buttons];
    int fds[num_buttons];
    const char gpioValLocations[num_buttons][256];
    int i, j;
    for (i = 0; i < num_buttons ; i++){
        sprintf(gpioValLocations[i], "/sys/class/gpio/gpio%d/value", gpios[i]);
    }
    /* both of these array are meant to hold 1 char either "1" or "0" */
    char buffers[num_buttons][2];
    char prev_buffers[num_buttons][2];
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
    //TODO change POLLIN to POLLPRI if device has functional sysfs gpio interface
    //TODO if POLLPRI: change 3rd arg of poll() to -1
    for (;;) {
        poll(pfd, num_buttons, 1);
        /* wait for interrupt */
        for(i=0; i < num_buttons; i++){
            if ((pfd[i].revents & POLLIN)) {
                /* copy current values to compare to next to detected change */
                memcpy(prev_buffers[i], buffers[i],2);
                /* read new values */
                if (lseek(fds[i], 0, SEEK_SET) == -1) goto houstonWeHaveAProblem; /* one little goto is not the end of the world, please remain calm */
                if (read(fds[i], buffers[i], sizeof buffers[i]) == -1) goto houstonWeHaveAProblem; /* world's over... */
                if (atoi(prev_buffers[i]) != atoi(buffers[i])) {
                    // place fn call here
                    if (first_press == true){
                        // TODO first_press must be a bool array (one for each button)
                        // TODO we need a thread array of single and long press
                        first_press = false;
                        //spawn two threads here
                        pthread_create(&threads[2], NULL, single_press, NULL);
                        pthread_create(&threads[3], NULL, long_press, NULL);
                    }else if(atoi(buffers[i]) == 1){
                        // output double tap
//                        first_press = true; /* reset the press indicator */
                    }
                    LOGD("change detected");
                }
            }
        }
    }
    /* shutdown */
    houstonWeHaveAProblem:
    LOGD("Reading Terminated");
    for (i = 0; i < num_buttons; i++){
        close(fds[i]);
    }
    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
 }

void *single_press(){
    JNIEnv* singleEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&singleEnv,&args);
//    for(;;){
//        LOGD("single is running");
//        sleep(1);
//    }
    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
};

void *long_press(){
    JNIEnv* longEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&longEnv,&args);
// run a timer and if not killed by single press then o/p long press
//    for(;;){
//        LOGD("long is running");
//        sleep(1);
//    }
    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
};

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


