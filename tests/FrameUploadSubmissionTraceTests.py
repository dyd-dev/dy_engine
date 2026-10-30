"""Check real --benchmark-wsi-timing output: one native upload+graph submit per warm frame."""
import argparse
import collections
import csv

parser=argparse.ArgumentParser()
parser.add_argument('trace')
parser.add_argument('--warmup',type=int,default=300)
parser.add_argument('--frames',type=int,default=600)
args=parser.parse_args()
with open(args.trace,encoding='utf-8-sig') as stream:
    events=list(csv.DictReader(line for line in stream if not line.startswith('#')))
counts=collections.Counter(int(event['frame']) for event in events
                           if event['event']=='Vulkan queue submit' and event['result']=='0')
for frame in range(args.warmup+1,args.warmup+args.frames+1):
    if counts[frame]!=1:
        raise SystemExit(f'FAIL frame {frame}: expected one upload+graph native submission, got {counts[frame]}')
print(f'PASS {args.frames} measured frames each used one upload+graph native submission')
