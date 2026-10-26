Texture2D hdrImage:register(t0,space0);
SamplerState hdrSampler:register(s1,space0);
// values/settings.xyzw는 노출 배율·감마 적용·RGBE 내보내기·톤 매핑 우회다.
// whiteContrast는 LMS 배율/대비, liftSaturation은 lift/채도, gammaSharpen은 RGB 감마/선명도, gainEnabled는 gain/색보정 활성, outputSize.xy는 출력 픽셀 간격이다.
cbuffer Settings:register(b15,space0) {float4 settings;float4 whiteContrast;float4 liftSaturation;float4 gammaSharpen;float4 gainEnabled;float4 outputSize;};
// RGB를 LMS 응답으로 옮겨 채널별 백색 보정 배율을 곱한 뒤 RGB로 되돌린다. 되돌린 음수 성분은 0으로 제한한다.
float3 OutputWhiteBalance(float3 color)
{
    if(all(whiteContrast.xyz==float3(1,1,1)))return color; // 화이트밸런스 배율이 모두 1이면 행렬 왕복 오차를 만들지 않고 원래 RGB를 반환한다.
    float3 lms=float3(dot(color,float3(.390405,.549941,.00892632)),
        dot(color,float3(.0708416,.963172,.00135775)),dot(color,float3(.0231082,.128021,.936245)));
    lms*=whiteContrast.xyz;
    return max(float3(dot(lms,float3(2.85847,-1.62879,-.024891)),
        dot(lms,float3(-.210182,1.1582,.000324281)),dot(lms,float3(-.041812,-.118169,1.06867))),float3(0,0,0));
}
// gain·lift → RGB별 역감마 → 0.18 기준 대비 → 휘도와 섞는 채도 순서다. 최종값은 표시 범위 0~1로 제한한다.
float3 OutputColorGrade(float3 color)
{
    if(gainEnabled.w<.5)return color; // 색보정이 꺼져 있으면 gain·lift·감마·대비·채도를 생략한다.
    color=clamp(color*gainEnabled.rgb+liftSaturation.rgb,float3(0,0,0),float3(1,1,1));
    if(any(gammaSharpen.rgb!=float3(1,1,1)))
        color=pow(color,float3(1,1,1)/max(gammaSharpen.rgb,float3(.01,.01,.01)));
    color=(color-float3(.18,.18,.18))*whiteContrast.w+float3(.18,.18,.18);
    float luminance=dot(color,float3(.2126,.7152,.0722));
    return clamp(lerp(float3(luminance,luminance,luminance),color,liftSaturation.w),float3(0,0,0),float3(1,1,1));
}
// 노출을 곱하고 큰 값을 제한한 뒤 HDR/(HDR+1)로 압축한다. 색보정이 켜져 있으면 압축 전 화이트밸런스와 압축 후 색보정을 적용한다.
float3 OutputMapHdr(float3 hdr)
{
    if(settings.w>.5)return clamp(hdr,float3(0,0,0),float3(1,1,1)); // 톤 매핑 우회 요청이면 노출과 색보정 없이 입력을 0~1로 제한한다.
    hdr=min(max(hdr*settings.x,float3(0,0,0)),float3(16777216.0,16777216.0,16777216.0));
    if(gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr); // 색보정 활성 상태에서만 HDR 색에 화이트밸런스를 적용한다.
    hdr=min(hdr,float3(16777216.0,16777216.0,16777216.0));
    return OutputColorGrade(hdr/(hdr+float3(1,1,1)));
}
// RGBE 내보내기 요청이면 인코딩 결과를 바로 반환하고, 그 외에 톤 매핑을 진행한다. 선명도 계산은 중심과 이웃에 같은 톤 매핑·색보정을 적용한 값을 사용한다.
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    float4 hdr=hdrImage.SampleLevel(hdrSampler,uv,0);
    if(settings.z>.5) // RGBE 내보내기면 표시용 노출·색보정·선명도·감마를 모두 건너뛴다.
    {
        // RGBE로 인코딩할 RGB를 0~65504로 제한한 뒤 가장 큰 성분을 구한다. 표시용 보정은 위 분기에서 우회한다.
        float3 radiance=clamp(hdr.rgb,float3(0,0,0),float3(65504,65504,65504));
        float peak=max(radiance.r,max(radiance.g,radiance.b));
        if(peak<=0)return float4(0,0,0,0); // 빛이 전혀 없는 픽셀은 로그를 계산하지 않고 네 채널 0으로 기록한다.
        // 가장 밝은 RGB 성분이 들어갈 2의 거듭제곱을 골라 RGB를 나누고, 지수에 128을 더해 A 채널로 저장한다. 원래 투명도 알파는 이 형식에 들어가지 않는다.
        float exponent=clamp(ceil(log2(peak)),-127.0,127.0);
        return float4(radiance/exp2(exponent),(exponent+128)/255);
    }
    float3 mapped=OutputMapHdr(hdr.rgb);
    if(gammaSharpen.w>0 && settings.w<.5) // 선명도 강도가 양수이고 톤 매핑을 우회하지 않은 일반 출력에만 이웃 필터를 적용한다.
    {
        // 색보정한 중심 색에서 이웃의 가중 평균을 뺀 값을 선명도 강도만큼 더한다.
        // 출력 픽셀 간격의 상하좌우 색을 읽는다. 중심과 휘도가 다른 이웃의 가중치를 줄이고, 선명도 결과를 중심과 네 이웃을 포함한 최소·최대 사이로 제한한다.
        const float2 offsets[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
        float3 minimum=mapped,maximum=mapped,blur=float3(0,0,0);float weightSum=0;
        float luminance=dot(mapped,float3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            float3 neighbor=OutputMapHdr(hdrImage.SampleLevel(hdrSampler,uv+offsets[i]*outputSize.xy,0).rgb);
            float weight=1/(1+abs(dot(neighbor,float3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(settings.y>.5)mapped=pow(mapped,float3(1.0/2.2,1.0/2.2,1.0/2.2)); // 출력 감마가 요청된 경로만 1/2.2 거듭제곱을 적용한다. 타깃의 색 공간 설정과 함께 결정된다.
    return float4(mapped,hdr.a);
}
