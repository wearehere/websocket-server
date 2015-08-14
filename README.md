# websocket-server

##What is websocket-server

base on (https://github.com/m8rge/cwebsocket)

add multi user support

add cgi like script support


##Usage

####compile

just run compile.sh

####run:

websocket -p port -a authscript -r runscript

######authscript

the command to check if url is valid

It will be invoked with 2 params, username first, passwd second

and should output "true" or "false" to standard output

######runscript 

the script handle other text message

It will be invoked by 1 param, text message

and should output any response to standard output

##sample

####auth script
<pre><code>
#!/bin/sh
user=$1
passwd=$2
if [ user = "abc" ] then
   echo "true"
else
   echo "false"
fi
</code></pre>

####runscript
<pre><code>
\#!/bin/sh
echo $1
</code></pre>


##contact

* Mail(64029000#qq.com)

