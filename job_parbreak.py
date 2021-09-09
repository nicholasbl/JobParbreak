#!/usr/bin/env python3

'''
MIT License

Copyright (c) 2021 Nicholas Brunhart-Lupo

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
'''

import argparse
import asyncio
from asyncio.tasks import sleep
import datetime
import json
import uuid
import sys

KEY_PENDING = "pending"
KEY_IN_WORK = "in_work"
KEY_DONE = "done"
KEY_FAILED = "failed"

JSON_NEXT = "next"
JSON_COMM = "command"
JSON_MESS = "reason"

JSON_STDOUT = "stdout"
JSON_STDERR = "stderr"

BLANK = json.dumps({}).encode()


class Node:
    '''
    A class to represent a job node.

    Attributes
    ----------
    command : str
        command to execute
    status : str
        string representation of status (one of KEY_*)
    '''

    def __init__(self, command) -> None:
        '''Constructor, takes a command string to store'''
        self.command = command
        self.status = KEY_PENDING


class APrompt:
    '''
    A class to handle state for an async command prompt

    Attributes
    ----------
    loop : asyncio event loop
        hosting event loop
    queue : asyncio.Queue
        text queue from stdin
    '''

    def __init__(self) -> None:
        self.loop = asyncio.get_event_loop()
        self.queue = asyncio.Queue()
        self.loop.add_reader(sys.stdin, self.on_input)

    def on_input(self):
        '''Used to extract text from stdin and place in a queue'''
        asyncio.ensure_future(self.queue.put(
            sys.stdin.readline()), loop=self.loop)

    async def __call__(self, message):
        '''Print a message and await input from the user'''
        print(message, end='', flush=True)
        return (await self.queue.get()).rstrip("\n")


class Server:
    '''
    A class to represent a job server.

    Each job is represented by a UUID key.

    Attributes
    ----------
    server_start_time : datetime
        the time the server started, used for timing (in future)
    server_command_db : map of str uuids -> Node
        database of all jobs known to the server
    server_command_pending_list : list of str uuids
        list of jobs pending, to avoid having to keep scanning the db
    server_link : an asyncio TCP server object
        reference to the server object, needed for issuing a shutdown, etc
    prompt : APrompt
        the async prompt object we use to get commands from the console    
    '''

    def __init__(self, opt_file) -> None:
        '''Initialize the server object with an optional command list text file object'''
        self.server_start_time = datetime.datetime.now()
        self.server_command_pending_list = []
        self.server_command_db = {}
        self.server_link = None
        self.prompt = APrompt()

        loop = asyncio.get_event_loop()
        loop.create_task(self.run_prompt())

        if opt_file is not None:
            lines = opt_file.readlines()

            self.server_load_list(lines)

    def server_load_list(self, lines):
        '''Add lines of commands to the database and to the pending list'''
        self.server_start_time = datetime.datetime.now()
        print("Loading {} commands...".format(len(lines)))

        for line in lines:
            next = str(uuid.uuid4())
            self.server_command_pending_list.append(next)
            self.server_command_db[next] = Node(line.strip())

    async def server_get_next(self):
        '''
        Get the next command to issue to clients.

        If there is none, sleep for 5 seconds and check again
        '''
        while True:
            # print("Getting next task...")
            if not len(self.server_command_pending_list):
                await asyncio.sleep(5)
                continue
            id = self.server_command_pending_list.pop()
            assert(self.server_command_db[id].status == KEY_PENDING)
            self.server_command_db[id].status = KEY_IN_WORK
            to_send = {JSON_NEXT: id,
                       JSON_COMM: self.server_command_db[id].command}
            return json.dumps(to_send), id

    def server_handle_incoming(self, b):
        '''Handle returning data from the client; start, success, fail'''
        try:
            read_obj = json.loads(b)
        except:
            print("Unable to parse json!")
            return

        if not len(read_obj):
            # skip blank objects, as they are used by a client to ask for a new job
            return

        if KEY_DONE in read_obj:
            key = read_obj[KEY_DONE]
            r = KEY_DONE

        if KEY_FAILED in read_obj:
            key = read_obj[KEY_FAILED]
            r = KEY_FAILED

            print(f"Job {key} failed!")

            try:
                fail_o = json.loads(read_obj[JSON_MESS])
                print(f"stdout: {fail_o[JSON_STDOUT]}")
                print(f"stderr: {fail_o[JSON_STDERR]}")
            except:
                pass

        if key not in self.server_command_db:
            print("Got a 'done' for a job we aren't in control of!")
            return

        assert(self.server_command_db[key].status == KEY_IN_WORK)
        self.server_command_db[key].status = r

    async def server_cb(self, reader, writer):
        '''Created as a new task whenever a new client connects'''
        assigned = None

        while True:
            data = await reader.read(2048)
            data = data.decode()

            self.server_handle_incoming(data)

            to_send, assigned = await self.server_get_next()

            writer.write(to_send.encode())
            await writer.drain()

    async def c_progress(self, l):
        '''User is asking for progress info'''
        to_do = [k for k, v in self.server_command_db.iter() if v.status ==
                 KEY_IN_WORK or v.status == KEY_PENDING]

        prs = "{} left".format(len(to_do))
        print("Progress: ", prs)

    async def c_add(self, l):
        '''User is asking to add a new file containing commands'''
        try:
            with open(l[0]) as f:
                lines = f.readlines()
                self.server_load_list(lines)
        except:
            print("Unable to load file")

    async def c_clear(self, l):
        '''User wants to clear all pending jobs'''
        self.server_command_pending_list.clear()

    async def c_exit(self, l):
        '''User wants to close this server down'''
        self.server_link.close()
        await self.server_link.wait_closed()

    async def run_prompt(self):
        '''Handle user input'''
        prompt_decode = {
            "progress": self.c_progress,
            "add": self.c_add,
            "clear": self.c_clear,
            "exit": self.c_exit,
        }

        await asyncio.sleep(2)
        while True:
            text = await self.prompt(">>> ")
            parts = text.split()
            if not len(parts):
                continue

            k = parts[0]
            if k not in prompt_decode:
                continue

            try:
                await prompt_decode[k](parts[1:])
            except:
                pass


async def as_server(command_file, port):
    '''Run as a server, creating server state and loops'''
    job_server = Server(command_file)

    server = await asyncio.start_server(job_server.server_cb, port=port)

    job_server.server_link = server

    address = server.sockets[0].getsockname()
    print("Serving on: ", address)

    try:
        async with server:
            await server.serve_forever()
    except asyncio.exceptions.CancelledError:
        print("Closing down...")


async def client_do_work(c, id):
    '''Handle a new job command, creating a subprocess, etc...'''
    print("Got job: ", id)
    print("Running command: ", c)
    proc = await asyncio.create_subprocess_shell(c, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)

    stdout, stderr = await proc.communicate()

    print("Done: ", proc.returncode)

    ret = {}
    ret[JSON_MESS] = json.dumps(
        {JSON_STDOUT: str(stdout), JSON_STDERR: str(stderr)})

    rcode = KEY_DONE if proc.returncode == 0 else KEY_FAILED

    ret[rcode] = id

    return json.dumps(ret)


async def as_client(serveraddy, port):
    '''Run as a client, creating state and loops'''
    reader, writer = await asyncio.open_connection(serveraddy, port)

    writer.write(BLANK)
    await writer.drain()

    while True:
        try:
            data = await reader.read(2048)
            data = data.decode()
            data = json.loads(data)

            id = data[JSON_NEXT]
            cm = data[JSON_COMM]

            result = await client_do_work(cm, id)

            writer.write(result.encode())
            await writer.drain()

        except:
            print("Done.")
            return


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='A simple job distribution system')
    parser.add_argument('-s', '--server', action="store_true",
                        help='act as a server')

    parser.add_argument('-t', '--txtfile', metavar="FILE", type=open,
                        help='serve jobs from the given text file')

    parser.add_argument('-c', '--client', metavar="ADDRESS", type=str,
                        help='act as a client, and connect to server')
    parser.add_argument('-p', '--port', metavar="PORT", type=int, default=55000,
                        help='port to use. if given as client, overrides port in provided url')

    args = parser.parse_args()

    if args.server and args.client:
        print("Unable to be server and client at the same time!")
        exit(1)

    if args.server:
        loop = asyncio.get_event_loop()
        loop.run_until_complete(as_server(args.txtfile, args.port))

    if args.client:
        asyncio.run(as_client(args.client, args.port))
