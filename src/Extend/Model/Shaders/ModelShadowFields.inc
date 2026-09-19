// 기본 shadow 입력의 바이너리 배치와 맞추되 기본 구현의 내부 헤더는 참조하지 않는다.
    DY_SHADOW_MATRIX lightViewProjectionMatrix[128];
    DY_SHADOW_FLOAT4 atlasRect[128];
    DY_SHADOW_FLOAT4 directionalViews[4];
    DY_SHADOW_FLOAT4 pointViews[16];
    DY_SHADOW_FLOAT4 spotViews[16];
    DY_SHADOW_FLOAT4 directionalSplits[4];
    DY_SHADOW_MATRIX cameraView;
    DY_SHADOW_FLOAT4 filterParams;
