# execvars

## Overview

The execvars service can be used to associated executable commands with
system variables and execute those commands when a request is made
to print the system variable's value.

The service runs and registers for a PRINT notification for each
execvar it manages.  When another client (eg getvar), requests
the variable server to print the value of a variable that is
managed by the execvar service, the varserver will send a signal
to the execvar service to render the variable's value.
The execvar service will then execute the pre-defined command associated
with the execvar service and pipe the results to the requesting
client's output stream.

## Prerequisites

The execvars service requires the following components:

- varserver : variable server ( https://github.com/tjmonk/varserver )
- tjson : JSON parser library ( https://github.com/tjmonk/libtjson )
- varcreate : create variables from a JSON file ( https://github.com/tjmonk/varcreate )

## Example configuration file

The execvars configuration file lists the execvars managed by the execvar service
and associates a CLI command(s) with the variable to be executed when the
variable is rendered.

```
{
    "commands" : [
        { "var" : "/sys/network/mac",
          "exec" : "ifconfig eth0 | grep ether | awk {'printf \"%s\",$2'}" },
        { "var" : "/sys/info/uptime",
          "exec" : "uptime | tr '\\n' '\\0'" },
        { "var" : "/sys/network/ip",
          "exec" : "ifconfig | grep broadcast | awk {'printf \"%s\",$2'}"}
    ]
}
```

In the example above, the `/sys/network/mac` variable will extract the system MAC address
from the output of the ifconfig command for eth0.  The `/sys/info/uptime` variable will get
the system uptime using the uptime command.  The `/sys/network/ip` variable will get the
system IP address from the ifconfig command.

Note: on your system you may need to install the ifconfig command as follows:

```
sudo apt-get install net-tools
```

## Build / Install

```
$ ./build.sh
```

## Test

### Start the varserver

```
$ varserver &
```

### Create the execvars

```
$ varcreate test/vars.json
```

### Start an instance of the execvars service

```
$ execvars -f test/execvars.json &
```

### Query the execvars

```
$ vars -vn /NETWORK/
/SYS/NETWORK/IP=172.17.0.4
/SYS/NETWORK/MAC=02:42:ac:11:00:04

$ getvar /sys/info/uptime
17:34:16 up 19:27,  0 users,  load average: 0.11, 0.27, 0.26
```





