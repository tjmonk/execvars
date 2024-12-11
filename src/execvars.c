/*============================================================================
MIT License

Copyright (c) 2023 Trevor Monk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
============================================================================*/

/*!
 * @defgroup execvars execvars
 * @brief Map variables to executable commands
 * @{
 */

/*==========================================================================*/
/*!
@file execvars.c

    Execute Variables

    The execvars Application maps variables to command sequences
    using a JSON object definition to describe the mapping

    Variables and their command sequences are defined in
    a JSON object as follows:

    {
        "commands" : [
            { "var" : "/sys/network/mac",
              "exec" : "ifconfig eth0 | grep ether | awk {'print $2'}" },
            { "var" : "/sys/info/uptime",
              "exec" : "uptime" }
        ]
    }

    When the value of an exec variable is requested, the associated command
    is executed, and the response rendered to the specified output stream.

*/
/*==========================================================================*/

/*============================================================================
        Includes
============================================================================*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <varserver/varserver.h>
#include <tjson/json.h>
#include <sys/select.h>

/*============================================================================
        Private definitions
============================================================================*/

/*! execVar component which maps a system variable to a command sequence */
typedef struct execVar
{
    /*! variable handle */
    VAR_HANDLE hVar;

    /*! command sequence */
    char *pCmd;

    /*! pointer to the next exec variable */
    struct execVar *pNext;

} ExecVar;


/*! ExecVars state */
typedef struct execVarsState
{
    /*! variable server handle */
    VARSERVER_HANDLE hVarServer;

    /*! verbose flag */
    bool verbose;

    /*! timeout in seconds */
    int timeout_seconds;

    /*! name of the ExecVars definition file */
    char *pFileName;

    /*! pointer to the exec vars list */
    ExecVar *pExecVars;
} ExecVarsState;

/*============================================================================
        Private file scoped variables
============================================================================*/

/*! ExecVars State object */
ExecVarsState state;

/*============================================================================
        Private function declarations
============================================================================*/

void main(int argc, char **argv);
static int ProcessOptions( int argC, char *argV[], ExecVarsState *pState );
static void usage( char *cmdname );
static int SetupExecVar( JNode *pNode, void *arg );
static int ExecuteVar( ExecVarsState *pState,
                       VAR_HANDLE hVar,
                       int sig,
                       int fd );
static int ExecuteCommand( char *cmd, int fd, int timeout_seconds);
static void SetupTerminationHandler( void );
static void TerminationHandler( int signum, siginfo_t *info, void *ptr );

/*============================================================================
        Private function definitions
============================================================================*/

/*==========================================================================*/
/*  main                                                                    */
/*!
    Main entry point for the execvars application

    The main function starts the execvars application

    @param[in]
        argc
            number of arguments on the command line
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @return none

============================================================================*/
void main(int argc, char **argv)
{
    VARSERVER_HANDLE hVarServer = NULL;
    VAR_HANDLE hVar;
    int result;
    JNode *config;
    JArray *cmds;
    int sigval;
    int fd;
    int sig;
    char buf[BUFSIZ];

    /* clear the execvars state object */
    memset( &state, 0, sizeof( state ) );

    if( argc < 3 )
    {
        usage( argv[0] );
        exit( 1 );
    }

    /* set up an abnormal termination handler */
    SetupTerminationHandler();

    /* process the command line options */
    ProcessOptions( argc, argv, &state );

    /* process the input file */
    config = JSON_Process( state.pFileName );

    /* get the configuration array */
    cmds = (JArray *)JSON_Find( config, "commands" );

    /* get a handle to the VAR server */
    state.hVarServer = VARSERVER_Open();
    if( state.hVarServer != NULL )
    {
        /* set up the exec vars by iterating through the configuration array */
        JSON_Iterate( cmds, SetupExecVar, (void *)&state );

        while( 1 )
        {
            /* wait for a signal from the variable server */
            sig = VARSERVER_WaitSignal( &sigval );
            if( sig == SIG_VAR_PRINT )
            {
                /* open a print session */
                VAR_OpenPrintSession( state.hVarServer,
                                      sigval,
                                      &hVar,
                                      &fd );

                /* execute the variable */
                ExecuteVar( &state, hVar, sig, fd );

                /* Close the print session */
                VAR_ClosePrintSession( state.hVarServer,
                                       sigval,
                                       fd );
            }
        }

        /* close the variable server */
        VARSERVER_Close( state.hVarServer );
    }
}

/*==========================================================================*/
/*  SetupExecVar                                                            */
/*!
    Set up an execvar object

    The SetupExecVar function is a callback function for the JSON_Iterate
    function which sets up an exec variable from the JSON configuration.
    The exec variable definition object is expected to look as follows:

    { "var": "varname", "exec": "<command sequence>" }

    @param[in]
       pNode
            pointer to the ExecVar node

    @param[in]
        arg
            opaque pointer argument used for the execvar state object

    @retval EOK - the exec variable was set up successfully
    @retval EINVAL - the exec variable could not be set up

============================================================================*/
static int SetupExecVar( JNode *pNode, void *arg )
{
    ExecVarsState *pState = (ExecVarsState *)arg;
    JVar *pName;
    JVar *pCommandString;
    char *varname = NULL;
    char *cmd = NULL;
    VARSERVER_HANDLE hVarServer;
    ExecVar *pExecvar;
    int result = EINVAL;

    if( pState != NULL )
    {
        /* get a handle to the VarServer */
        hVarServer = pState->hVarServer;
        pName = (JVar *)JSON_Find( pNode, "var" );
        if( pName != NULL )
        {
            varname = pName->var.val.str;
        }

        pCommandString = (JVar *)JSON_Find( pNode, "exec" );
        if( pCommandString != NULL )
        {
            cmd = pCommandString->var.val.str;
        }

        if( ( varname != NULL ) &&
            ( cmd != NULL ) )
        {
            /* allocate memory for the exec variable */
            pExecvar = malloc( sizeof( ExecVar ) );
            if( pExecvar != NULL )
            {
                /* get a handle to the exec var */
                pExecvar->hVar = VAR_FindByName( hVarServer, varname );

                /* set the command associated with the exec var */
                pExecvar->pCmd = strdup( cmd );

                /* tell the variable server that we will be responsible
                   for fulfilling print requests for this exec var */
                result = VAR_Notify( hVarServer,
                                     pExecvar->hVar,
                                     NOTIFY_PRINT );

                /* store the execvar into the execvar list */
                pExecvar->pNext = pState->pExecVars;
                pState->pExecVars = pExecvar;
            }
        }
    }

    return result;
}

/*==========================================================================*/
/*  ExecuteVar                                                              */
/*!
    Execute an execvar

    The ExecuteVar function iterates through all the registered execvars
    looking for the specified variable handle.  If found, the command
    string associated with it is executed, and the output piped to the
    specified output stream.

    @param[in]
       pState
            pointer to the ExecVars state object

    @param[in]
        hVar
            handle of the variable to execute

    @param[in]
        sig
            type of signal:  one of:
                SIG_VAR_PRINT
                SIG_VAR_MODIFIED

    @param[in]
        fd
            output file descriptor to pipe the command output to

    @retval EOK - variable executed successfully
    @retval ENOENT - variable was not found
    @retval EINVAL - invalid arguments

============================================================================*/
static int ExecuteVar( ExecVarsState *pState,
                       VAR_HANDLE hVar,
                       int sig,
                       int fd )
{
    int result = EINVAL;
    ExecVar *pExecVar;
    FILE *fp_in;
    char buf[BUFSIZ];

    if( ( pState != NULL ) &&
        ( hVar != VAR_INVALID ) )
    {
        result = ENOENT;

        pExecVar = pState->pExecVars;
        while( pExecVar != NULL )
        {
            if( pExecVar->hVar == hVar )
            {
                if( pExecVar->pCmd != NULL )
                {
                    if ( sig == SIG_VAR_PRINT )
                    {
                        result = ExecuteCommand( pExecVar->pCmd, fd, pState->timeout_seconds );
                    }
                    else
                    {
                        result = ENOTSUP;
                    }
                }
                else
                {
                    result = ENOTSUP;
                }
                break;
            }

            pExecVar = pExecVar->pNext;
        }
    }

    return result;
}

/*==========================================================================*/
/*  popen2                                                                  */
/*!
    This is a slightly modified version of the popen function which
    allows the PID of the command to be returned.
    The original can be found at:
    https://www.cse.lehigh.edu/%7Ebrian/course/2013/cunix/notes/ch11/popen.c

    The popen function opens a pipe to a command and returns a file
    pointer to the command output stream.

    @param[in]
       command
            pointer to the NUL terminated command string to execute

    @param[in]
        mode
            pointer to the NUL terminated mode string
            "r" for read, "w" for write

    @param[out]
        pid
            pointer to the process id of the command

    @retval FILE * - file pointer to the command output stream
    @retval NULL - command could not be executed

============================================================================*/
FILE *popen2( const char *command, const char *mode, pid_t *pid )
{
    const int READ = 0;
    const int WRITE = 1;

    int pfp[2];     /* the pipe and the process */
    FILE *fp;       /* fdopen makes a fd a stream */
    int parent_end, child_end;  /* of pipe */

    if( *mode == 'r' )
    {
        /* figure out direction */
        parent_end = READ;
        child_end = WRITE;
    }
    else if( *mode == 'w' )
    {
        parent_end = WRITE;
        child_end = READ;
    }
    else
    {
        return NULL;
    }

    if( pipe( pfp ) == -1 )
    {
        /* get a pipe */
        return NULL;
    }

    if( ( *pid = fork() ) == -1 )
    {
        /* and a process */
        close( pfp[0] ); /* or dispose of pipe */
        close( pfp[1] );
        return NULL;
    }

    /* --------------- parent code here ------------------- */
    /* need to close one end and fdopen other end */

    if( *pid > 0 )
    {
        if( close( pfp[child_end] ) == -1 )
        {
            return NULL;
        }
        return fdopen( pfp[parent_end], mode ); /* same mode */
    }

    /* --------------- child code here --------------------- */
    /* need to redirect stdin or stdout then exec the cmd */

    if( close( pfp[parent_end] ) == -1 )
    {
        /* close the other end */
        exit( 1 ); /* do NOT return */
    }

    if( dup2( pfp[child_end], child_end ) == -1 )
    {
        exit( 1 );
    }

    if( close( pfp[child_end] ) == -1 )
    {
        /* done with this one */
        exit( 1 );
    }

    /* all set to run cmd */
    execl( "/bin/sh", "sh", "-c", command, NULL );
    exit( 1 );
}

/*==========================================================================*/
/*  ExecuteCommand                                                          */
/*!
    Execute a command and pipe the output to the output stream

    The ExecuteCommand function executes the specified command
    and redirects the command output to the specified output stream

    @param[in]
       cmd
            pointer to the NUL terminated command string to execute

    @param[in]
        fd
            output file descriptor to pipe the command output to

    @param[in]
        timeout_seconds
            timeout in seconds, if it is 0, the command is executed
            in the current process, otherwise, a new process is forked

    @retval EOK - command executed successfully
    @retval ENOENT - the command was not found
    @retval EINVAL - invalid arguments

============================================================================*/
static int ExecuteCommand( char *cmd, int fd, int timeout_seconds )
{
    int n;
    int result = EINVAL;
    int retval;
    char buf[BUFSIZ];
    FILE *fp_in;
    int pipefd;
    fd_set readfds;
    struct timeval timeout;
    int pid;

    if( cmd != NULL )
    {
        /* assume command not executed until popen succeeds */
        result = ENOENT;

        /* only fork a process if timeout is needed */
        if( timeout_seconds <= 0 )
        {
            /* execute the command */
            fp_in = popen( cmd, "r" );
            if( fp_in != NULL )
            {
                do
                {
                    /* read a buffer of output */
                    n = fread( buf, 1, BUFSIZ, fp_in );
                    if( n > 0 )
                    {
                        if( fd >= 0 )
                        {
                            /* set the output to the output stream */
                            write( fd, buf, n );
                        }
                    }
                } while( n > 0 );

                /* close the command output data stream */
                pclose( fp_in );

                /* indicate success */
                result = EOK;
            }
        }
        else
        {
            /* execute the command */
            fp_in = popen2( cmd, "r", &pid );
            if( fp_in != NULL )
            {
                /* get the file descriptor to use later with kill */
                pipefd = fileno( fp_in );
                if( pipefd >= 0 )
                {
                    /* Set up the timeout context for select */
                    FD_ZERO( &readfds );
                    FD_SET( pipefd, &readfds );
                    timeout.tv_sec = timeout_seconds;
                    timeout.tv_usec = 0;

                    do
                    {
                        retval = select( pipefd + 1, &readfds, NULL, NULL, &timeout );
                        if( retval < 0 )
                        {
                            /* select error */
                            result = EINVAL;
                        }
                        else
                        {
                            if( retval == 0 )
                            {
                                /* timeout occurred, kill the process */
                                result = EINVAL;
                                kill( pid, SIGKILL );
                                syslog( LOG_ERR, "Timeout %d seconds exceeded for command %s\n", timeout_seconds, cmd );
                            }
                            else
                            {
                                /* read a buffer of output */
                                n = fread( buf, 1, BUFSIZ, fp_in );
                                if( n > 0 )
                                {
                                    if( fd >= 0 )
                                    {
                                        /* set the output to the output stream */
                                        write( fd, buf, n );
                                    }
                                }
                                else
                                {
                                    if( n == 0 )
                                    {
                                        /* end of data, exit now */
                                        retval = 0;
                                        result = EOK;
                                    }
                                    else
                                    {
                                        /* error reading data */
                                        result = EINVAL;
                                    }
                                }
                            }
                        }
                    } while( retval > 0 );
                }
                else
                {
                    /* error getting file descriptor */
                    result = EINVAL;
                }
            }

            /* close the command output data stream in any case */
            pclose( fp_in );
        }
    }

    return result;
}

/*==========================================================================*/
/*  usage                                                                   */
/*!
    Display the application usage

    The usage function dumps the application usage message
    to stderr.

    @param[in]
       cmdname
            pointer to the invoked command name

    @return none

============================================================================*/
static void usage( char *cmdname )
{
    if( cmdname != NULL )
    {
        fprintf(stderr,
                "usage: %s [-v] [-h] [-t <timeout>] -f <filename>\n"
                " [-h] : display this help\n"
                " [-v] : verbose output\n"
                " [-t] : timeout in seconds (will create a new process for every exec call)\n"
                " -f <filename> : configuration file\n",
                cmdname );
    }
}

/*==========================================================================*/
/*  ProcessOptions                                                          */
/*!
    Process the command line options

    The ProcessOptions function processes the command line options and
    populates the ExecVarState object

    @param[in]
        argC
            number of arguments
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @param[in]
        pState
            pointer to the ExecVars state object

    @return none

============================================================================*/
static int ProcessOptions( int argC, char *argV[], ExecVarsState *pState )
{
    int c;
    int result = EINVAL;
    const char *options = "hvt:f:";

    if( ( pState != NULL ) &&
        ( argV != NULL ) )
    {
        while( ( c = getopt( argC, argV, options ) ) != -1 )
        {
            switch( c )
            {
                case 'v':
                    pState->verbose = true;
                    break;

                case 'h':
                    usage( argV[0] );
                    break;

                case 'f':
                    pState->pFileName = strdup(optarg);
                    break;

                case 't':
                    pState->timeout_seconds = atoi(optarg);
                    break;

                default:
                    break;

            }
        }
    }

    return 0;
}

/*==========================================================================*/
/*  SetupTerminationHandler                                                 */
/*!
    Set up an abnormal termination handler

    The SetupTerminationHandler function registers a termination handler
    function with the kernel in case of an abnormal termination of this
    process.

============================================================================*/
static void SetupTerminationHandler( void )
{
    static struct sigaction sigact;

    memset( &sigact, 0, sizeof(sigact) );

    sigact.sa_sigaction = TerminationHandler;
    sigact.sa_flags = SA_SIGINFO;

    sigaction( SIGTERM, &sigact, NULL );
    sigaction( SIGINT, &sigact, NULL );

}

/*==========================================================================*/
/*  TerminationHandler                                                      */
/*!
    Abnormal termination handler

    The TerminationHandler function will be invoked in case of an abnormal
    termination of this process.  The termination handler closes
    the connection with the variable server and cleans up its VARFP shared
    memory.

@param[in]
    signum
        The signal which caused the abnormal termination (unused)

@param[in]
    info
        pointer to a siginfo_t object (unused)

@param[in]
    ptr
        signal context information (ucontext_t) (unused)

============================================================================*/
static void TerminationHandler( int signum, siginfo_t *info, void *ptr )
{
    syslog( LOG_ERR, "Abnormal termination of execvars\n" );
    VARSERVER_Close( state.hVarServer );
    exit( 1 );
}

/*! @}
 * end of execvars group */
