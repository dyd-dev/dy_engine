//
//  ViewController.h
//  AdvancedMetalIOS
//
//  작성: 정준혁 · 2026-10-16
//

#import <UIKit/UIKit.h>
#include <stdbool.h>

typedef int (*DYExampleEntry)(int, char**, const char*, const char*, void (*)(bool));

@interface ViewController : UIViewController

+ (void)setExampleEntry:(DYExampleEntry)entry;

@end
