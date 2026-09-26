# Run by example.python_repl.runs-alone-from-an-empty-directory: the REPL alone
# in an empty directory, with the standard library served from inside it.
import asyncio
import email.message
import json
import sqlite3
import ssl
import sys

message = email.message.EmailMessage()
message["Subject"] = "standalone"
print("stdlib:", json.__spec__.origin, sys.path, asyncio.run(asyncio.sleep(0, "ran")), message["Subject"])
