//
//  SceneDelegate.m
//  AdvancedMetalIOS
//
//  작성: 정준혁 · 2026-10-16
//

#import "SceneDelegate.h"

@interface SceneDelegate ()

@end

@implementation SceneDelegate


- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    // Main 스토리보드가 UIWindow와 ViewController를 자동 연결하므로 수동 생성은 하지 않는다.
}


- (void)sceneDidDisconnect:(UIScene *)scene {
    // 시스템이 화면 연결을 해제할 때 호출된다. 세션은 이후 다시 연결될 수 있다.
}


- (void)sceneDidBecomeActive:(UIScene *)scene {
    // 화면 활성화 알림이다. 렌더링 재개는 ViewController의 앱 상태 알림에서 처리한다.
}


- (void)sceneWillResignActive:(UIScene *)scene {
    // 화면 비활성화 직전 알림이다. 렌더링 중지는 ViewController의 앱 상태 알림에서 처리한다.
}


- (void)sceneWillEnterForeground:(UIScene *)scene {
    // 화면이 전경으로 돌아올 때 호출된다. 현재 별도의 복원 처리는 없다.
}


- (void)sceneDidEnterBackground:(UIScene *)scene {
    // 화면이 배경으로 이동할 때 호출된다. 현재 별도의 상태 저장은 하지 않는다.
}


@end
