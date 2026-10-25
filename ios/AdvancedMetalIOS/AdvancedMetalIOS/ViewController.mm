#import "ViewController.h"
#import <QuartzCore/CAMetalLayer.h>
#import <CoreText/CoreText.h>
#include "Platform/Windows/IOSApplication.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using dyf::Platform::InputEvent;
using dyf::Platform::InputEventType;
using dyf::Platform::InputAction;
using dyf::Platform::MouseButton;
using dyf::Platform::Key;

@interface MetalView : UIView {
    __weak UITouch* _primaryTouch;
    CGPoint _origin, _previous;
    BOOL _scrolling;
}
@property(nonatomic, copy) void (^sendInput)(const InputEvent&);
@property(nonatomic) BOOL exampleRunning;
@end

static Key PhysicalKey(UIKeyboardHIDUsage usage) {
    const int value=int(usage);
    if(value>=4 && value<=29) return Key(int(Key::A)+value-4);
    if(value>=30 && value<=38) return Key(int(Key::Num1)+value-30);
    if(value>=58 && value<=69) return Key(int(Key::F1)+value-58);
    switch(value) {
    case 39:return Key::Num0; case 40:return Key::Enter; case 41:return Key::Escape;
    case 42:return Key::Backspace; case 43:return Key::Tab; case 44:return Key::Space;
    case 45:return Key::Minus; case 46:return Key::Equal;
    case 74:return Key::Home; case 75:return Key::PageUp; case 77:return Key::End;
    case 78:return Key::PageDown; case 79:return Key::Right; case 80:return Key::Left;
    case 81:return Key::Down; case 82:return Key::Up;
    case 224:return Key::LeftControl; case 225:return Key::LeftShift;
    case 226:return Key::LeftAlt; case 227:return Key::LeftSuper;
    case 228:return Key::RightControl; case 229:return Key::RightShift;
    case 230:return Key::RightAlt; case 231:return Key::RightSuper;
    default:return Key::Unknown;
    }
}

@implementation MetalView
+ (Class)layerClass { return CAMetalLayer.class; }
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)cursor:(CGPoint)point {
    InputEvent event;
    event.type = InputEventType::Cursor;
    event.x = point.x; event.y = point.y;
    if(self.sendInput) self.sendInput(event);
}
- (void)button:(InputAction)action {
    InputEvent event;
    event.type = InputEventType::MouseButton;
    event.code = int(MouseButton::Left); event.action = action;
    if(self.sendInput) self.sendInput(event);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    if(_primaryTouch) return;
    _primaryTouch = touches.anyObject;
    _origin = _previous = [_primaryTouch locationInView:self];
    _scrolling = NO;
    [self cursor:_origin];
    if(self.exampleRunning) [self button:InputAction::Press];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    if(!_primaryTouch || ![touches containsObject:_primaryTouch]) return;
    const auto point = [_primaryTouch locationInView:self];
    [self cursor:point];
    if(!self.exampleRunning) {
        if(std::hypot(point.x-_origin.x, point.y-_origin.y)>8) _scrolling=YES;
        if(_scrolling && self.sendInput) {
            InputEvent scroll; scroll.type=InputEventType::Scroll;
            scroll.y=(point.y-_previous.y)/40.0;
            self.sendInput(scroll);
        }
    }
    _previous=point;
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    if(!_primaryTouch || ![touches containsObject:_primaryTouch]) return;
    [self cursor:[_primaryTouch locationInView:self]];
    if(self.exampleRunning) [self button:InputAction::Release];
    else if(!_scrolling) {
        // Delay a menu click until touch-up so a swipe only scrolls the list.
        [self button:InputAction::Press];
        [self button:InputAction::Release];
    }
    _primaryTouch=nil;
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)touches; (void)event;
    if(_primaryTouch && self.exampleRunning) [self button:InputAction::Release];
    _primaryTouch=nil;
}
- (void)keys:(NSSet<UIPress*>*)presses action:(InputAction)action {
    for(UIPress* press in presses) {
        if(!press.key || !self.sendInput) continue;
        const auto key=PhysicalKey(press.key.keyCode);
        if(key==Key::Unknown) continue;
        InputEvent event; event.type=InputEventType::Key; event.code=int(key); event.action=action;
        self.sendInput(event);
    }
}
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event; [self keys:presses action:InputAction::Press];
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event; [self keys:presses action:InputAction::Release];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event; [self keys:presses action:InputAction::Release];
}
@end

@interface ViewController () {
    std::shared_ptr<dyf::Platform::IOSApplication> _application;
    std::thread _runner;
    UIButton* _backButton;
    UILabel* _errorLabel;
    BOOL _started, _active, _stopping;
}
- (void)setExampleRunning:(BOOL)running;
@end

static __weak ViewController* activeController;
static DYExampleEntry exampleEntry;
static void ExampleChanged(bool running) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [activeController setExampleRunning:running];
    });
}
static std::string SystemFontPath() {
    CTFontRef font=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,18,CFSTR("en"));
    if(!font) return {};
    NSURL* url=CFBridgingRelease(CTFontCopyAttribute(font,kCTFontURLAttribute));
    CFRelease(font);
    return url.isFileURL && url.path.UTF8String ? std::string(url.path.UTF8String) : std::string{};
}

@implementation ViewController
+ (void)setExampleEntry:(DYExampleEntry)entry {
    exampleEntry=entry;
}
- (void)loadView {
    self.view=[[MetalView alloc] initWithFrame:CGRectZero];
    self.view.backgroundColor=UIColor.blackColor;
    self.view.multipleTouchEnabled=YES;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    _active=UIApplication.sharedApplication.applicationState==UIApplicationStateActive;
    _backButton=[UIButton buttonWithType:UIButtonTypeSystem];
    [_backButton setTitle:@"‹ Examples" forState:UIControlStateNormal];
    [_backButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    _backButton.backgroundColor=[UIColor colorWithWhite:0 alpha:.75];
    _backButton.layer.cornerRadius=8;
    _backButton.hidden=YES;
    _backButton.accessibilityIdentifier=@"returnToExamples";
    _backButton.translatesAutoresizingMaskIntoConstraints=NO;
    [_backButton addTarget:self action:@selector(backToExamples:) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:_backButton];
    [NSLayoutConstraint activateConstraints:@[
        [_backButton.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:12],
        [_backButton.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8],
        [_backButton.widthAnchor constraintEqualToConstant:112],
        [_backButton.heightAnchor constraintEqualToConstant:40]
    ]];
    __weak ViewController* weakSelf=self;
    ((MetalView*)self.view).sendInput=^(const InputEvent& event) {
        ViewController* owner=weakSelf;
        if(owner && owner->_application) owner->_application->PushInput(event);
    };
    auto* center=NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(pauseRendering:) name:UIApplicationWillResignActiveNotification object:nil];
    [center addObserver:self selector:@selector(resumeRendering:) name:UIApplicationDidBecomeActiveNotification object:nil];
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation { return UIInterfaceOrientationLandscapeRight; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self setNeedsUpdateOfSupportedInterfaceOrientations];
    [self.view setNeedsLayout];
}
- (void)publishWindowState {
    if(!_application) return;
    const auto bounds=self.view.bounds.size;
    const float scale=float(self.view.contentScaleFactor);
    _application->Update({int(bounds.width),int(bounds.height)}, {scale,scale},
        _active, self.view.window && !self.view.window.hidden);
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if(!self.view.window) return;
    self.view.contentScaleFactor=self.view.window.screen.scale;
    [self publishWindowState];
    if(!exampleEntry || _started || self.view.bounds.size.width<=0 || self.view.bounds.size.height<=0) return;
    _started=YES;
    _stopping=NO;
    [_errorLabel removeFromSuperview];
    _errorLabel=nil;
    try {
        const std::string font=SystemFontPath();
        if(font.empty()) throw std::runtime_error("The system font has no readable font file.");
        NSURL* assets=[NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"examples" isDirectory:YES];
        const std::string assetRoot=assets.path.UTF8String;
        std::vector<std::string> arguments;
        for(NSString* value in NSProcessInfo.processInfo.arguments)
            arguments.emplace_back(value.UTF8String ? value.UTF8String : "");
        if(arguments.empty()) arguments.emplace_back("dyf_examples");
        _application=std::make_shared<dyf::Platform::IOSApplication>((__bridge void*)self.view);
        activeController=self;
        [self publishWindowState];
        [self.view becomeFirstResponder];
        const auto application=_application;
        const auto entry=exampleEntry;
        ViewController* owner=self; // Retain the controller/view until worker cleanup completes.
        _runner=std::thread([application,owner,entry,arguments=std::move(arguments),assetRoot,font]() mutable {
            int result=1;
            std::string error;
            try {
                result=application->Run([&] {
                    std::vector<char*> argv;
                    for(auto& value:arguments) argv.push_back(value.data());
                    argv.push_back(nullptr);
                    return entry(int(arguments.size()),argv.data(),assetRoot.c_str(),font.c_str(),ExampleChanged);
                });
            } catch(const std::exception& exception) { error=exception.what(); }
            std::fprintf(stderr,"Application exited with code %d\n",result);
            std::fflush(stderr);
            // All engine resources and synchronous main-queue calls are finished.
            // Only now may the UIKit thread join the already-finishing worker.
            dispatch_async(dispatch_get_main_queue(), ^{
                if(owner->_runner.joinable()) owner->_runner.join();
                owner->_application.reset();
                owner->_started=NO;
                [owner setExampleRunning:NO];
                if(activeController==owner) activeController=nil;
                if(result!=0 && !owner->_stopping)
                    [owner showError:error.empty() ? "The examples runner failed." : error.c_str()];
            });
        });
    } catch(const std::exception& error) {
        _application.reset();
        _started=NO;
        [self showError:error.what()];
    }
}
- (void)setExampleRunning:(BOOL)running {
    ((MetalView*)self.view).exampleRunning=running;
    _backButton.hidden=!running;
    [self.view bringSubviewToFront:_backButton];
}
- (void)backToExamples:(id)sender {
    (void)sender;
    if(_application) _application->RequestClose();
}
- (void)pauseRendering:(NSNotification*)notification {
    (void)notification; _active=NO; [self publishWindowState];
}
- (void)resumeRendering:(NSNotification*)notification {
    (void)notification; _active=YES; [self publishWindowState];
}
- (void)showError:(const char*)message {
    NSLog(@"dy_engine examples: %s",message);
    [_errorLabel removeFromSuperview];
    _errorLabel=[[UILabel alloc] initWithFrame:CGRectZero];
    _errorLabel.text=[NSString stringWithUTF8String:message];
    _errorLabel.textColor=UIColor.whiteColor;
    _errorLabel.numberOfLines=0;
    _errorLabel.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:_errorLabel];
    [NSLayoutConstraint activateConstraints:@[
        [_errorLabel.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:20],
        [_errorLabel.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-20],
        [_errorLabel.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}
- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    _stopping=YES;
    if(_application) _application->Stop();
}
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    // The worker retains this controller and releases it after the main-thread
    // completion has joined it; deallocation never waits on a live worker.
}
@end
