#!/usr/bin/env python
##########################################################################
# run/ec2-setup/spot.py
#
# Part of Project Thrill - http://project-thrill.org
#
# Copyright (C) 2015 Matthias Stumpp <mstumpp@gmail.com>
#
# All rights reserved. Published under the BSD-2 license in the LICENSE file.
##########################################################################

import boto3
import time
import json
import datetime
import sys

with open('config.json') as data_file:
    data = json.load(data_file)

client = boto3.client('ec2')
ec2 = boto3.resource('ec2')

job_id = int(time.time())


blockMappings = [{'DeviceName': '/dev/sda1',
                 'Ebs': {
                    'VolumeSize': 8,
                    'DeleteOnTermination': True,
                    'VolumeType': 'gp2'
                 }
               }]

if data["VOL_SNAPSHOT_ID"]:
    blockMappings.append(
        {
            'DeviceName': data["DEVICE"],
            'Ebs': {
                'SnapshotId': data["VOL_SNAPSHOT_ID"],
                'DeleteOnTermination': True,
                'VolumeType': 'gp2'
            }
         })

response = client.request_spot_instances(SpotPrice=data["SPOT_PRICE"],
                                       InstanceCount=data["COUNT"],
                                       Type=data["TYPE"],
                                       #ValidFrom=datetime.datetime(2015, 10, 11, 18, 10, 00),
                                       ValidUntil=datetime.datetime(2015, 10, 11, 19, 37, 00),
                                       LaunchSpecification={
                                            'ImageId' : data["AMI_ID"],
                                            'KeyName' : data["EC2_KEY_HANDLE"],
                                            'InstanceType' : data["INSTANCE_TYPE"],
                                            'SecurityGroups' : [ data["SECGROUP_HANDLE"] ],
                                            'Placement' : { 'AvailabilityZone': data["ZONE"]
                                            'BlockDeviceMappings' : blockMappings}
                                       })

request_ids = []
for request in response['SpotInstanceRequests']:
    request_ids.append(request['SpotInstanceRequestId'])

fulfilled_instances = []
loop = True;

print "waiting for instances to get fulfilled..."
while loop:
    requests = client.describe_spot_instance_requests(SpotInstanceRequestIds=request_ids)
    for request in requests['SpotInstanceRequests']:
        if request['State'] in ['closed', 'cancelled', 'failed']:
            print request['SpotInstanceRequestId'] + " " + request['State']
            loop = False
            break; # TODO(ms) ensure running instances are terminated
        if 'InstanceId' in request and request['InstanceId'] not in running_instances:
           fulfilled_instances.append(request['InstanceId'])
           print request['InstanceId'] + " running..."
    if len(fulfilled_instances) == int(data["COUNT"]):
        print 'all requested instances are fulfilled'
        break;
    time.sleep(5)

if loop == False:
    print "unable to fulfill all requested instances... aborting..."
    sys.exit();

# add tag to each instance
for instance in fulfilled_instances:
    instance.create_tags(Tags=[{'Key': 'JobId', 'Value': str(job_id)}])

# ensure all instances are running
loop = True;
while loop:
    loop = False
    response = client.describe_instance_status(InstanceIds=running_instances, IncludeAllInstances=True)
    for status in response['InstanceStatuses']:
        if status['InstanceState']['Name'] != 'running':
            loop = True

print "all instances are running..."

print str(data["COUNT"]) + " instances up and running! JobId: " + str(job_id)

##########################################################################
