struct Output {float4 position:SV_Position;float2 uv:TEXCOORD0;};

// 정점 버퍼 없이 정점 번호 0·1·2로 전체 화면 삼각형과 UV를 만들어 톤 매핑할 HDR 이미지의 같은 위치를 읽게 한다.
Output main(uint id:SV_VertexID) {
    // 정점 번호의 비트로 UV (0,0), (2,0), (0,2)를 만들어 화면을 덮는 큰 삼각형을 구성한다.
    Output o;o.uv=float2((id<<1)&2,id&2);

    // UV에 (2,-2)를 곱하고 (-1,1)을 더해 투영 후 좌표를 만든다. 삼각형은 화면 밖까지 뻗지만 화면 안에서는 UV가 0~1을 덮어 HDR 이미지 전체를 읽는다.
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;

}
