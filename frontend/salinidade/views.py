from django.http import HttpResponse
from django.shortcuts import render
from rest_framework import generics
from .models import Leitura
from .serializers import LeituraCreateSerializer
import json
from django.core.serializers.json import DjangoJSONEncoder

class LeituraCreateAPIView(generics.CreateAPIView):
    queryset = Leitura.objects.all()
    serializer_class = LeituraCreateSerializer

def index(request):
    qs = (Leitura.objects
          .values("id", "sensor_id", "tipo", "valor", "data_hora")
          .order_by("data_hora"))
    return render(request, "front/index.html", {"leituras": list(qs)})
