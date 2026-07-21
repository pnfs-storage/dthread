/*
 * dt-testwrap  wrapper for dthread test programs
 * 17-Jun-2026  chuck@ece.cmu.edu
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/wait.h>

/*
 * generate a newly malloced string from the given parts.
 * part list must end in a NULL (for stdarg to know when to stop).
 * return size of malloced buffer (including the \0), or 0 on error.
 * caller is responsible for freeing the returned string.
 */
size_t strgen(char **newstr, ...) {
    va_list ap;
    size_t newlen;
    char *n, *nstr, *part;

    /* pass 1: compute length we need */
    va_start(ap, newstr);
    newlen = 1;   /* include byte for null termination */
    while ((part = va_arg(ap, char *)) != NULL) {
         newlen += strlen(part);
    }
    va_end(ap);

    /* pass 2: malloc and assemble new string */
    n = nstr = malloc(newlen);    /* includes space for null */
    if (!nstr)
        return(0);
    va_start(ap, newstr);
    while ((part = va_arg(ap, char *)) != NULL) {
        while (*part) {
            *n++ = *part++;
        }
    }
    va_end(ap);
    *n = '\0';

    *newstr = nstr;
    return(newlen);
}

/* flags for fdforkprog */
#define FDFPROG_FIN	1	/* update child's stdin */
#define FDFPROG_FOUT	2	/* update child's stdout */
#define FDFPROG_FERR	4	/* update child's stderr */
#define FDFPROG_EDUPOUT 8	/* make child's stderr a dup of its stdout */
#define FDFPROG_FIO     3       /* short for FIN|FOUT */
#define FDFPROG_FIOE    7       /* short for FIN|FOUT|FERR */
#define FDFPROG_FIOD    11      /* short for FIN|FOUT|FEDUPOUT */

/* optional fdforkprog callback function pointer */
typedef void (*fdforkprog_cb)(void *arg);

/*
 * set/clear fd's close on exec flag.   return 0 on success, -1 on fail.
 */
static int fdcloexec(int fd, int state) {
    int flags;

    flags = fcntl(fd, F_GETFD);           /* get fd's current flags */
    if (flags == -1) {
        warn("fcntl: F_GETFD");
        return(-1);
    }

    if (state)                            /* edit flag bits */
        flags |= FD_CLOEXEC;
    else
        flags &= ~FD_CLOEXEC;

    if (fcntl(fd, F_SETFD, flags) < 0) {  /* install new flags */
        warn("fcntl: F_SETFD");
        return(-1);
    }

    return(0);
}

/*
 * fdforkprog: fork program in a child process with stdio redirected
 * as specified.  for any pipes we create, we set the caller side
 * file descriptors to close-on-exec so that child only has the side
 * of the pipe it uses open.   return child pid on success, -1 on
 * failure.
 */
static pid_t fdforkprog(const char *prog, char *const argv[], int flags,
                       int *stdinfd, int *stdoutfd, int *stderrfd,
                       fdforkprog_cb fdf_cb, void *cbarg) {
    int lcv, pipefds[7];   /* [0,1]=in, [2,3]=out, [4,5]=err, 6=xtra err */
    char *emsg;
    pid_t child;
    int errfd;

    /*
     * sanity check args.  if a flag bit is NOT set, then its
     * corresponding fd pointer must be NULL.  thus, if a fd
     * pointer is NOT NULL, then its flag bit must be set.
     * also, we do not allow both FERR and EDUPOUT to be set
     * at the same time.
     */
    if ( ((flags & FDFPROG_FIN) == 0  && stdinfd) ||
         ((flags & FDFPROG_FOUT) == 0 && stdoutfd) ||
         ((flags & FDFPROG_FERR) == 0 && stderrfd) ||
         ((flags & FDFPROG_FERR) && (flags & FDFPROG_EDUPOUT)) ) {
        warnx("fdforkprog: usage error (%#x,%p,%p,%p)", flags, stdinfd,
              stdoutfd, stderrfd);
        return(-1);
    }

    /* convert non-null FERR/FOUT with stderrfd==stdoutfd to a EDUPOUT */
    if ((flags & FDFPROG_FERR) && (flags & FDFPROG_FOUT) &&
        stderrfd && stderrfd == stdoutfd) {
        flags &= ~FDFPROG_FERR;
        flags |= FDFPROG_EDUPOUT;
        stderrfd = NULL;    /* same as stdoutfd, so do dupout op */
    }

    /* set pipe fds to invalid value (-1) for close logic in 'error' label */
    for (lcv = 0; lcv < sizeof(pipefds)/sizeof(pipefds[0]) ; lcv++) {
        pipefds[lcv] = -1;
    }

    /*
     * first, create all the pipes we need with the parent side set
     * to close-on-exec so they don't leak out on any future forks.
     * note that pipe[0] is the read side of the pipe while pipe[1]
     * is the write side.
     *
     * if the flag bit for a stdio stream is not set, then we leave
     * that stream as-is.  if the stream's fd pointer arg is NULL,
     * we will close that stream in the child.   if stderr's EDUPOUT
     * flag is set, FERR must be clear (and stderrfd must be NULL).
     * in this case make the child's stderr a dup of its stdout.
     */
    if (stdinfd) {
        /* child reads stdin from [0], parent write to child stdin on [1] */
        if (pipe(&pipefds[0]) == -1 || fdcloexec(pipefds[1], 1) == -1) {
            warn("pipe for stdin failed");
            goto error;
        }
    }
    if (stdoutfd) {
        /* child writes stdout to [3], parent reads child's stdout from [2] */
        if (pipe(&pipefds[2]) == -1 || fdcloexec(pipefds[2], 1) == -1) {
            warn("pipe for stdout failed");
            goto error;
        }
    }
    if (stderrfd) {
        /* child writes stderr to [5], parent reads child's stderr from [4] */
        if (pipe(&pipefds[4]) == -1 || fdcloexec(pipefds[4], 1) == -1) {
            warn("pipe for stderr failed");
            goto error;
        }
    }

    /*
     * if the caller is changing stderr, dup the current stderr into
     * a second fd and mark it close on exec.  we'll use this to print
     * an error if execvp() failed (rather than dying silently).
     */
    if (flags & (FDFPROG_FERR|FDFPROG_EDUPOUT)) {
        if ((pipefds[6] = dup(fileno(stderr))) == -1) {
            warn("copy of stderr failed");
            goto error;
        }
        if (fdcloexec(pipefds[6], 1) == -1) {
            goto error;
        }
    }

    /*
     * now we can fork the child process...
     */
    child = fork();
    if (child == -1) {
        warn("fork");
        goto error;
    }

    /*
     * the child can now finish the I/O setup and exec the requested program.
     */
    if (child == 0) {

        if (fdf_cb != NULL) {        /* do user callback fn, if provided */
            (fdf_cb)(cbarg);            /* cb is allowed to exit on failure */
        }

        if (flags & FDFPROG_FIN) {   /* changing stdin? */
            if (stdinfd) {
                if (dup2(pipefds[0], STDIN_FILENO) < 0) {  /* dup to std */
                    err(1, "dup of pipe to stdin failed");
                }
                close(pipefds[0]);      /* close now dup'd copy */
                close(pipefds[1]);      /* close parent's side of pipe */
            } else {
                close(STDIN_FILENO);    /* no stdin for child */
            }
        }

        if (flags & FDFPROG_FOUT) {   /* changing stdout? */
            if (stdoutfd) {
                if (dup2(pipefds[3], STDOUT_FILENO) < 0) {  /* dup to std */
                    err(1, "dup of pipe to stdout failed");
                }
                close(pipefds[2]);      /* close parent's side of pipe */
                close(pipefds[3]);      /* close now dup'd copy */
            } else {
                close(STDOUT_FILENO);   /* no stdout for child */
            }
        }

        /* stderr handling, do dupout case first.  stdout is already set. */
        if (flags & FDFPROG_EDUPOUT) {
            if ((flags & FDFPROG_FOUT) && stdoutfd == NULL) {
                close(STDERR_FILENO);  /* dup no stdout to no stderr too */
            } else {
                if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
                    err(1, "failed to merge stdout and stderr");
                }
            }
        } else if (flags & FDFPROG_FERR) {
            if (stderrfd) {
                if (dup2(pipefds[5], STDERR_FILENO) < 0) {  /* dup to std */
                    err(1, "dup of pipe to stderr failed");
                }
                close(pipefds[4]);      /* close parent's side of pipe */
                close(pipefds[5]);      /* close now dup'd copy */
            } else {
                close(STDERR_FILENO);   /* no stderr for child */
            }
        }

        (void) execvp(prog, argv);

         /*
          * execvp() has failed, but we cannot print an error msg
          * to stderr because it may have been closed (in which case
          * a dup'd copy of stderr fd is in pipefds[6]).   figure
          * out where we can put an error message, and just use
          * write() to do it.
          */
         errfd = (pipefds[6] != -1) ? pipefds[6] : STDERR_FILENO;

        emsg = strerror(errno);
#define EWRITE(F,MSG) do {                                                    \
                        if (write(F, MSG, strlen(MSG)) < 0) exit(1);          \
                    } while (0);
        EWRITE(errfd, "fdforkprog exec: ");
        EWRITE(errfd, prog);
        EWRITE(errfd, ": ");
        EWRITE(errfd, emsg);
        EWRITE(errfd, " - child exiting!\n");
#undef EWRITE
        exit(1);
        /*NOTREACHED*/
    }

    /*
     * we are the parent.   finish setting up the file descriptors
     * and return success (for the fork, the exec can still fail later).
     */
    if (stdinfd) {             /* sanity check above ensured FIN is set */
        *stdinfd = pipefds[1];
        close(pipefds[0]);
    }
    if (stdoutfd) {
        *stdoutfd = pipefds[2];
        close(pipefds[3]);
    }
    if (stderrfd) {            /* cannot be EDUPOUT case */
        *stderrfd = pipefds[4];
        close(pipefds[5]);
    }
    if (pipefds[6] != -1)
        close(pipefds[6]);     /* don't need child's copy of stderr */

    /*
     * done, return child process.
     */
    return(child);

error:
    /* drop any fds we created */
    for (lcv = 0; lcv < sizeof(pipefds)/sizeof(pipefds[0]) ; lcv++) {
        if (pipefds[lcv] != -1) {
            close(pipefds[lcv]);
        }
    }
    return(-1);
}

/*
 * the core of the program where we run the test and process the output.
 * returns desired exit value.
 */
static int proctestcmd(pid_t child, int ignorefail, FILE *sfp, FILE *cfp) {
    char *sln, *cln, *look;
    size_t sln_sz, cln_sz;
    ssize_t rv;
    pid_t wrv;
    int lcv, cstatus, eval;

    /* getline() setup */
    sln = cln = NULL;
    sln_sz = cln_sz = 0;

    /* skip script handling if no script ... */
    if (sfp == NULL)
        goto no_script;

    /* run through each line of the script */
    while ( (rv = getline(&sln, &sln_sz, sfp)) > 0) {
        /* remove leading space */
        for (look = sln ; rv > 0 && isspace(*look) ; look++, rv--)
            /*null*/;
        /* remove trailing space */
        for (/*null*/ ; rv > 0 && isspace(look[rv-1]) ; rv--) {
            look[rv-1] = 0;
        }
        /* skip blank lines in script */
        if (rv < 1)
            continue;

        /* scan output for target line */
        while ( (rv = getline(&cln, &sln_sz, cfp)) > 0) {
            printf("%s", cln);      /* always print the output */

            if (strstr(cln, look))  /* break if target found */
                break;
        }

        if (rv < 1) {
            /* ignore child wait status, we've got an error */
            printf("\nERROR: script unable to find '%s'\n", look);
            if (sln) free(sln);
            if (cln) free(cln);
            return(1);
        }
    }

no_script:
    /*
     * finished script processing.  drain any remaining
     * command output and return success.
     */
    while ( (rv = getline(&cln, &sln_sz, cfp)) > 0) {
        printf("%s", cln);
    }
    printf("test EOF: test complete!\n");
    if (sln) free(sln);
    if (cln) free(cln);
    /* loop waitpid 5s in case child sent EOF but isn't fully exited yet */
    for (lcv = 0 ; lcv < 50 ; lcv++) {
         wrv = waitpid(child, &cstatus, WNOHANG);
         if (wrv)
             break;
         poll(NULL, 0, 100);  /* sleep 0.1 sec */
    }
    if (wrv != child) {
        if (wrv == 0)
            warnx("waitpid failed: returned 0!");
        else
            warn("waitpid failed");

        return(1);
    }
    if (WIFEXITED(cstatus)) {
        eval = WEXITSTATUS(cstatus);
        if (eval && ignorefail) {
            printf("NOTE: ignoring non-zero exit(%d) (ignorefail)\n", eval);
            return(0);
        }
        return(eval);
    }
    printf("waitpid: bad status %#x\n", cstatus);
    return(1);
}

static void usage(char *prog) {
    fprintf(stderr, "usage: %s [args] test-command [testargs]\n", prog);
    fprintf(stderr, "where args are:\n");
    fprintf(stderr, "\t-d dir   shm-file directory (def=/tmp)\n");
    fprintf(stderr, "\t-e       merge test-command stderr into stdout\n");
    fprintf(stderr, "\t-f file  shm-file name (def='dt.shm')\n");
    fprintf(stderr, "\t-i       ignore non-zero exit values\n");
    fprintf(stderr, "\t-k       keep shm-file at end\n");
    fprintf(stderr, "\t-p       tag shm-file with my PID\n");
    fprintf(stderr, "\t-s scr   check output using given script file\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "NOTE: we replace DT_SHM_PATH on command line with\n");
    fprintf(stderr, "      the actual filename we selected\n");
    exit(1);
}

int main(int argc, char **argv) {
    char *prog = argv[0];
    int ch, lcv, fdflags, cfd, rv;
    struct stat st;
    FILE *sfp, *cfp;
    pid_t cmdpid;

    /* default args */
    char *dt_shmdir = "/tmp";
    int dt_errmerge = 0;
    int dt_ignorefail = 0;
    char *dt_shmfile = "dt.shm";
    int dt_keep = 0;
    char *dt_script = NULL;

    char pidtag[16] = { 0 };
    char *dt_shm_path = NULL;     /* full path, generated by us */

    /*
     * parse our command line options
     *
     * XXX: need '+' in optstring with glibc so that it does not permute
     * the args.   we want to stop parsing opts when we hit a non-option
     * so that we can pass the rest to the test-command process as its
     * command line.  the '+' should be harmless on non-glibc getopts.
     * see discussion of POSIXLY_CORRECT in glib getopt man page.
     */
    while ((ch = getopt(argc, argv, "+d:ef:i:kps:")) != -1) {
        switch (ch) {
        case 'd':
            dt_shmdir = optarg;
            break;
        case 'e':
            dt_errmerge = 1;
            break;
        case 'f':
            dt_shmfile = optarg;
            break;
        case 'i':
            dt_ignorefail = 1;
            break;
        case 'k':
            dt_keep = 1;
            break;
        case 'p':
            snprintf(pidtag, sizeof(pidtag), ".%d", getpid());
            break;
        case 's':
            dt_script = optarg;
            break;
        default:
            usage(prog);
        }
    }
    argc -= optind;
    argv += optind;

    if (argc < 1) {
        fprintf(stderr, "missing test-command\n");
        usage(prog);
    }

    if (stat(dt_shmdir, &st) < 0)
        err(1, "dt_shmdir: %s", dt_shmdir);
    if ((st.st_mode & S_IFMT) != S_IFDIR)
        errx(1, "dt_shmdir: %s: Not a directory", dt_shmdir);

    if (strlen(dt_shmfile) == 0)
        errx(1, "dt_shmfile: empty filename");

    if (!dt_script) {
       sfp = NULL;
    } else {
        /* open script and prepare to read it */
        sfp = fopen(dt_script, "r");
        if (!sfp)
            err(1, "script-fopen: %s", dt_script);
    }

    /* generate dt_shm_path */
    if (strgen(&dt_shm_path, dt_shmdir, "/", dt_shmfile, pidtag, NULL) < 1)
        errx(1, "dt_shm_path: malloc failed");

    /* edit argv[] to sub in DT_SHM_PATH as needed */
    for (lcv = 0 ; lcv < argc ; lcv++) {
        if (strcmp(argv[lcv], "DT_SHM_PATH") == 0)
            argv[lcv] = dt_shm_path;
    }

    /* sanity check test command and then start it */
    if (access(argv[0], X_OK) < 0)       /* quick prelim check w/access */
        err(1, "test-command: %s", argv[0]);

    fdflags = FDFPROG_FOUT;
    if (dt_errmerge)
        fdflags |= FDFPROG_EDUPOUT;
    cmdpid = fdforkprog(argv[0], argv, fdflags, NULL, &cfd, NULL, NULL, NULL);
    if (cmdpid < 0)
        err(1, "fdforkprog: %s", argv[0]);
    if ((cfp = fdopen(cfd, "r")) == NULL)
        errx(1, "fdopen: %s", argv[0]);

    setlinebuf(stdout);
    rv = proctestcmd(cmdpid, dt_ignorefail, sfp, cfp);
    if (!dt_keep)
        unlink(dt_shm_path);

    exit(rv);
}
