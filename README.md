# JobParbreak
A super simple job distribution system in python.

The single script can be used as a server to host commands (such as blender render command lines) and can act as a client to execute them.

## Requirements
Just a working python >= 3.7 environment. The script requires no other packages

## Input
At the moment the input is a simple text file with shell commands to be executed. They can be passed to the script at startup or new files can be added while it is running.

The script does not do any environment handling, so it is recommended that paths be fully qualified, etc, and your shell command sets up the right variables that you need. Running on a shared filesystem (NFS) is the primary use-case.

## Security
None, this is just to automate what you would be typing anyway. You have to log in to each server/client you wish to use to run the script, so there's that security, I suppose.
