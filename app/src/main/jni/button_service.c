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
/* master and routine threads */
pthread_t run_threads[2];
/* single_press and long_press threads for each button */
pthread_t button_threads[2][2]; /* button_threads[i][2] where i = num_buttons */
/* gpio configuration functions */
void setup_gpios();
static int gpio_export(int pin);
static int set_edge(int pin, int edge);
/* threads */
void *master();
void *routine();
void *single_press(void *i);
void *long_press(void *i);
/* define a bool */
typedef enum {
    false,
    true
}bool;
static const int long_press_time = 5;
/* indicates a press in not yet a double tap */
bool first_press[2]; /* size must match number of buttons */
/* for use in taking the difference in times */
struct timeval beforeX;
struct timeval afterX;
struct timeval beforeY;
struct timeval afterY;
/* double used to have actual time when adding timeval.tv_sec + timeval.tv_usec
 * then take difference t2-t1 to find actual time */
double t1_long;
double t2_long;
double t1Y;
double t2Y;
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
    /* cls is made a global to be used in the spawned thread*/
    cls = (jclass)(*env)->NewGlobalRef(env,type);
    /* create the threads */
    pthread_create(&run_threads[0], NULL, master, NULL);
    pthread_create(&run_threads[1], NULL, routine, NULL);
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
    /* set the locations to correspond to the chosen pins */
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
    int button_ids[num_buttons]; /* threads must know which button they service */
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
                    button_ids[i]=i; //TODO check if this is too sketchy to have here
                    if (first_press[i] == true){
                        first_press[i] = false;
                        /* each button hit requires a single_press and long_press thread */
                        pthread_create(&button_threads[i][0], NULL, single_press, &button_ids[i]);
                        pthread_create(&button_threads[i][1], NULL, long_press, &button_ids[i]);
                    }else if(atoi(buffers[i]) == 1){
//                        LOGD("Double-Tap");
//                        first_press[i] = true; /* reset the press indicator */
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

void *single_press(void *i){
    JNIEnv* singleEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&singleEnv,&args);
    int button_id = *((int *) i);
    // if single press is o/p reset first_press[i]

    LOGD("single %d is running", button_id);


    (*jvm)->DetachCurrentThread(jvm);
    pthread_exit(NULL);
};

void *long_press(void *i){
    JNIEnv* longEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6; /* JNI version */
    args.name = NULL; /* thread name */
    args.group = NULL; /* thread group */
    (*jvm)->AttachCurrentThread(jvm,&longEnv,&args);
    gettimeofday(&beforeX,NULL);
    t1_long = beforeX.tv_sec + beforeX.tv_usec;
    int button_id = *((int *) i);
    LOGD("long %d is running", button_id);
// run a timer and if not killed then o/p long press
    while(1){
        gettimeofday(&afterX,NULL);
        t2_long = afterX.tv_sec + afterX.tv_usec;
        /* if key is held down for more than set time - long press */
        if ((t2_long-t1_long) > long_press_time){
            // output long-press
            LOGD("t1 is: %f", t1_long);
            LOGD("t2 is: %f", t2_long);
            LOGD("diff is: %f", (t2_long-t1_long));
            LOGD("long-press detected on %d", button_id);
            break;
        }
    }

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


