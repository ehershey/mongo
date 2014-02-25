#!/usr/bin/env python
#
# Generate and upload gpg signatures for files in S3
#
# Generate and upload detached gpg signatures for archive files in Amazon S3
#
# Requires standard MongoDB settings.py, like so:
# bucket = "downloads.mongodb.org"
# # downloads user
# id = "xxxxx"
# key = "xxxxx"
#
# Usage: s3sign.py [ --bucket <overridden s3 bucket> ] [ --notary-server <notary server> ] [ --filter <filter> ] [ --gpg-key-id <GPG key ID> ]
#


import argparse
import os
import sys

sys.path.append("." )
sys.path.append(".." )
sys.path.append("../../" )
sys.path.append("../../../" )

import simples3
import settings
import subprocess

# parse command line
#
parser = argparse.ArgumentParser(description='Sign MongoDB S3 Files')
parser.add_argument('--bucket', required = False, help='Override bucket in settings.py', default = settings.bucket);
parser.add_argument('--notary-server', required=False, help='Server for notary service', default = 'localhost');
parser.add_argument('--gpg-key-id', required=False, help='GPG Key ID to sign with', default = None);
parser.add_argument('--filter', required=False, 
                    help='Only sign files matching case-insensitive substring filter', default = None);
args = parser.parse_args()



# check s3 for pgp signatures

def check_dir( bucket , prefix ):

    zips = {}
    sigs = {}
    for ( key , modify , etag , size ) in bucket.listdir( prefix=prefix ):
        if args.filter and args.filter.lower() not in key.lower():
            pass
        elif key.endswith(".tgz" ) or key.endswith(".zip" ) or key.endswith(".tar.gz" ) or key.endswith("md5"):
            # generate signature
            zips[key] = etag.replace( '"' , '' )
        elif key.endswith(".sig" ) or key.endswith(".asc" ):
            sigs[key] = True
        elif key.endswith(".msi" ):
            pass
        elif key.find("$folder$" ) > 0:
            pass
        else:
            print("unknown file type: " + key )

    for x in zips:
        m = x + ".sig"
        if m in sigs:
            continue

        print("need to do: " + x + " " + zips[x] + " to " + m )
        # bucket.put( m , zips[x] , acl="public-read" )


def run():

    bucket = simples3.S3Bucket( args.bucket , settings.id , settings.key )

    for x in [ "osx" , "linux" , "win32" , "sunos5" , "src" ]:
        check_dir( bucket , x )


if __name__ == "__main__":
    run()
