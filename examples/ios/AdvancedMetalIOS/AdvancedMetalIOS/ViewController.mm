// iOS 실행 연결: UIKit 레이아웃 확정 → 예제 생성 → CADisplayLink → Example::Tick().
// 데스크톱과 iOS는 같은 예제 렌더러를 사용하며, 이 파일은 화면·선택·앱 상태 전환을 담당한다.
#import "ViewController.h"
#import <QuartzCore/CAMetalLayer.h>
#include "Example.h"
#include "dyf/Platform/Window.h"
#include <vector>

// iOS 스왑체인은 UIView의 기존 CAMetalLayer를 사용하므로 기본 레이어 클래스를 교체한다.
@interface MetalView : UIView
@end
@implementation MetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface ViewController () {
    std::unique_ptr<dyf::Platform::Window> _engineWindow;
    std::unique_ptr<advanced::Example> _example;
    CADisplayLink* _displayLink;
    BOOL _started;
    UISegmentedControl* _exampleSelector;
    UILabel* _errorLabel;
}
@end

@implementation ViewController
- (void)loadView {
    self.view = [[MetalView alloc] initWithFrame:CGRectZero];
    self.view.backgroundColor = UIColor.blackColor;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // 휴대폰에서 앱을 다시 실행하지 않고 예제를 선택한다.
    _exampleSelector = [[UISegmentedControl alloc] initWithItems:@[@"07 PBR", @"08 그림자"]];
    _exampleSelector.selectedSegmentIndex = 0;
    _exampleSelector.accessibilityIdentifier = @"exampleSelector";
    _exampleSelector.backgroundColor = [UIColor colorWithWhite:0 alpha:0.7];
    _exampleSelector.selectedSegmentTintColor = UIColor.systemBlueColor;
    [_exampleSelector setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.whiteColor}
                                   forState:UIControlStateNormal];
    [_exampleSelector addTarget:self action:@selector(changeExample:)
              forControlEvents:UIControlEventValueChanged];
    _exampleSelector.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_exampleSelector];
    [NSLayoutConstraint activateConstraints:@[
        [_exampleSelector.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8],
        [_exampleSelector.centerXAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerXAnchor],
        [_exampleSelector.widthAnchor constraintEqualToConstant:260],
        [_exampleSelector.heightAnchor constraintEqualToConstant:36]
    ]];
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(pauseRendering:)
                   name:UIApplicationWillResignActiveNotification object:nil];
    [center addObserver:self selector:@selector(resumeRendering:)
                   name:UIApplicationDidBecomeActiveNotification object:nil];
}

// 기존 예제의 가로 구도를 보여준다. 픽셀을 늘여서 화면을 채우지 않는다.
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation { return UIInterfaceOrientationLandscapeRight; }
- (BOOL)prefersStatusBarHidden { return YES; }

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    UIWindowSceneGeometryPreferencesIOS* preferences = [[UIWindowSceneGeometryPreferencesIOS alloc]
        initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [self.view.window.windowScene requestGeometryUpdateWithPreferences:preferences errorHandler:^(NSError* error) {
        NSLog(@"가로 화면 전환: %@", error.localizedDescription);
    }];
}

// 초기 세로 크기로 스왑체인을 만들지 않도록 가로 레이아웃까지 기다린다.
// 이후 크기 변경은 MetalDevice::BeginFrame에서 반영하므로 여기서는 한 번만 생성한다.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if(!self.view.window) return;
    self.view.contentScaleFactor = self.view.window.screen.scale;
    if(_started || self.view.bounds.size.width <= self.view.bounds.size.height) return;
    _started = YES;
    try {
        std::string selected = "07";
        std::vector<std::string> arguments{"AdvancedMetalIOS"};
        // --example은 앱이 소비하고, 나머지는 기존 예제의 argc/argv 파서로 넘긴다.
        NSArray<NSString*>* processArguments = NSProcessInfo.processInfo.arguments;
        for(NSUInteger i = 1; i < processArguments.count; ++i) {
            NSString* argument = processArguments[i];
            if([argument isEqualToString:@"--example"]) {
                if(++i >= processArguments.count)
                    throw std::runtime_error("--example requires 07 or 08.");
                selected = processArguments[i].UTF8String;
            } else {
                arguments.emplace_back(argument.UTF8String);
                // 상대 캡처 경로는 쓰기 가능한 앱의 Documents 폴더에 저장한다.
                if([argument isEqualToString:@"--capture"] && i + 1 < processArguments.count) {
                    NSString* path = processArguments[++i];
                    if(!path.isAbsolutePath) {
                        NSURL* documents = [NSFileManager.defaultManager
                            URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
                        path = [documents.path stringByAppendingPathComponent:path];
                    }
                    arguments.emplace_back(path.UTF8String);
                }
            }
        }
        if(selected != "07" && selected != "08")
            throw std::runtime_error("--example must be 07 or 08.");
        _exampleSelector.selectedSegmentIndex = selected == "07" ? 0 : 1;
        [self startExample:arguments];
    } catch(const std::exception& error) {
        [self showError:error.what()];
    }
}

// 예제별 옵션은 첫 실행에만 적용하고, 버튼 전환 시에는 해당 예제의 기본값을 쓴다.
- (void)changeExample:(UISegmentedControl*)sender {
    (void)sender;
    try {
        [self startExample:std::vector<std::string>{"AdvancedMetalIOS"}];
    } catch(const std::exception& error) {
        [self showError:error.what()];
    }
}

- (void)startExample:(std::vector<std::string>)arguments {
    [_displayLink invalidate];
    _displayLink = nil;
    // 같은 CAMetalLayer를 재사용하므로 이전 콜백과 GPU 자원을 정리한 뒤 새 장치를 연결한다.
    _example.reset();
    [_errorLabel removeFromSuperview];
    _errorLabel = nil;
    std::vector<char*> argv;
    for(auto& argument : arguments) argv.push_back(argument.data());
    if(!_engineWindow)_engineWindow=std::make_unique<dyf::Platform::Window>(
        dyf::Platform::NativeWindowHandle{dyf::Platform::NativeWindowKind::UIKit,(__bridge void*)self.view});
    // UIView의 소유권은 UIKit에 둔다. 생성 함수는 화면 주소와 실행 옵션을 동기적으로 받는다.
    if(_exampleSelector.selectedSegmentIndex == 0)
        _example = advanced::CreatePBRLighting(_engineWindow->GetHandle(), int(argv.size()), argv.data());
    else
        _example = advanced::CreateShadowMapping(_engineWindow->GetHandle(), int(argv.size()), argv.data());
    if(!_example) return;
    NSLog(@"advanced %02ld: Metal renderer initialized", (long)_exampleSelector.selectedSegmentIndex + 7);
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(drawFrame:)];
    _displayLink.preferredFramesPerSecond = 60;
    _displayLink.paused = UIApplication.sharedApplication.applicationState != UIApplicationStateActive;
    [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

// while 반복문 대신 iOS의 화면 갱신 콜백마다 한 프레임만 실행한다.
- (void)drawFrame:(CADisplayLink*)sender {
    (void)sender;
    try {
        dyf::Platform::Window::PollEvents();
        if(!_example->Tick()) {
            [_displayLink invalidate];
            _displayLink = nil;
            NSLog(@"advanced: requested frames completed");
        }
    } catch(const std::exception& error) {
        [self showError:error.what()];
    }
}

// 비활성화 중 새 프레임은 제출하지 않고, 이미 제출한 GPU 작업은 완료시킨다.
- (void)pauseRendering:(NSNotification*)notification {
    (void)notification;
    _displayLink.paused = YES;
    try {
        if(_example) _example->WaitIdle();
    } catch(const std::exception& error) {
        [self showError:error.what()];
    }
}

- (void)resumeRendering:(NSNotification*)notification {
    (void)notification;
    _displayLink.paused = NO;
}

- (void)showError:(const char*)message {
    [_displayLink invalidate];
    _displayLink = nil;
    _example.reset();
    NSLog(@"AdvancedMetalIOS: %s", message);
    [_errorLabel removeFromSuperview];
    UILabel* label = [[UILabel alloc] initWithFrame:CGRectZero];
    _errorLabel = label;
    label.text = [NSString stringWithFormat:@"실행 실패\n%s", message];
    label.textColor = UIColor.whiteColor;
    label.numberOfLines = 0;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:20],
        [label.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-20],
        [label.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_displayLink invalidate];
    _displayLink = nil;
    _example.reset();
    _started = NO;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_displayLink invalidate];
}
@end
