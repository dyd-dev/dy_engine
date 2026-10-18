//
//  AppDelegate.m
//  AdvancedMetalIOS
//
//  작성: 정준혁 · 2026-10-16
//

#import "AppDelegate.h"

@interface AppDelegate ()

@end

@implementation AppDelegate


- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // 앱 시작을 승인한다. 렌더링 초기화는 ViewController가 담당한다.
    return YES;
}


#pragma mark - UISceneSession lifecycle


- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession options:(UISceneConnectionOptions *)options {
    // 새 화면 세션에 Info.plist의 기본 구성(Main 스토리보드·SceneDelegate)을 연결한다.
    return [[UISceneConfiguration alloc] initWithName:@"Default Configuration" sessionRole:connectingSceneSession.role];
}


- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions {
    // 화면 세션 폐기 알림이다. 현재 앱은 여기서 정리할 별도 세션 자원이 없다.
}


@end
