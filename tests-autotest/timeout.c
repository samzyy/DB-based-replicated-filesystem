#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

/** @file
 * 
 * Run a test for a given amount of runtime until either it quits naturally or we are forced to kill it.
 *
 * This file was written for the regression-testing of the nagios/ldap and the fsc-fs work which I
 * was doing on my own inspired by the needs of a company I worked for in 2007.  The function
 * fork/execs a child process, and sets an alarm.  If the child quits, all is fine; if the alarm goes
 * off, it kills the child in a relatively impolite manner: SIGTERM, wait 1 sec, SIGKILL.  There's a
 * bit of tongue-in-cheek here, but that's the basics.
 * 
 * family_assassination() is the function tied to the SIGALRM; the wait for a normal termination is
 * done with a wait() or pause().
 *
 * This file is permitted use as GPL2 or Attribution + ShareAlike (by-sa) Creative-Commons. 
 * Whichever works for you.
 */

/** global for the money-shot: the pid that is killed when the alarm goes off */
int _pid = -1;

/** When triggered, kills the process at _pid
 *
 * @param ignored (ignored)
 */
void family_assassination (int ignored)
{
    (void) printf ("%s\n", "timeout");
    if (-1 != _pid) kill (_pid, SIGTERM);
    if (-1 != _pid) sleep(1);
    if (-1 != _pid) kill (_pid, SIGKILL);
    exit(1);
}

/**
 * Main function: set up the alarm and run the child process until it quits or the alarm fires.
 *
 * This function reads the parameters, set up a SIGALRM to call family_assassination() (kill your
 * child, get it?  dark humour, I'm sorry), then lauunch the child process an wait for it to finish. 
 * If the alarm goes off before the child finishes, this program kills the child and itself like a
 * gruesome story in the newspaper; if the child finishes, then we terminate normally, which is
 * nearly as gruesome (again, sorry).
 *
 * @return 1 if there's a parameter problem or exec() problem, or if the child was killed by alarm
 * @return 0 if there are no problems and child completed normally
 */

int main (int argc, char *argv[])
{
    char opt;
    char testing = 1;
    char background = 1;
    unsigned sleeper = 1;
    struct sigaction timeout_sigaction =
    {
        .sa_handler = family_assassination
    };

    (void) sigaction (SIGALRM, &timeout_sigaction, NULL);

    while (-1 != (opt = getopt(argc, argv, "Bt:")))
    switch (opt)
    {
        case 'B': /* background for the daemon */
            background = 1;
            break;
        case 't': /* timeout */
            sleeper = atoi(optarg);
            testing = 0;
            break;
        case '?': /* unknown; ignored */
	    fprintf (stderr, "unknown option\n");
	    return -2;
    }

    if (optind >= argc)
       return 1;
    /*
     * parent returns immediately with a good return to allow backgrounding daemons (themselves
     * non-backgrounding) so that the parent test-runner can continue, letting the test daemon be
     * killed on timeout
     */
    else if (0 < background)
    {
        switch (fork())
        {
            case 0: /* child */
                break;
            default: /* parent */
                return 0;
        }
    }

    /* else */
    switch (_pid = fork())
    {
        case 0: /* child */
            if (testing)
            {
                printf ("argc %d ; optind %d; argv[optind] [%s]\n", argc, optind, argv[optind]);
                sleep (5+sleeper);
            }
            else
            {
                execvp (argv[optind], &argv[optind]);
                return 1;
            }
            
            return 0;

        default: /* parent */
            /* set the morning wakeup */
            alarm (sleeper);

            /* and snooze for the child forked pid */
            //(void) wait (NULL);
            /* and wait for SIGCHLD or SIGALRM */
            (void) pause();

            /* if we got here, the pid finished */
            return 0;
    }
}
