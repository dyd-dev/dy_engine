//
//  main.m
//  AdvancedMetalIOS
//
//  작성: 정준혁 · 2026-10-16
//

#import <UIKit/UIKit.h>
#import "AppDelegate.h"

// UIApplicationMain이 이벤트 루프를 소유한다. Main 스토리보드의 ViewController가 예제를 시작한다.
// 렌더링 반복문은 여기에 두지 않고 ViewController의 CADisplayLink로 구동한다.
int main(int argc, char * argv[]) {
    NSString * appDelegateClassName;
    @autoreleasepool {
        // 앱 시작 전에 생성한 임시 Objective-C 객체를 이 풀에서 정리한다.
        appDelegateClassName = NSStringFromClass([AppDelegate class]);
    }
    return UIApplicationMain(argc, argv, nil, appDelegateClassName);
}
