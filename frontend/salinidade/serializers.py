# seu_app/serializers.py
from rest_framework import serializers
from .models import Leitura, Sensor

class LeituraCreateSerializer(serializers.ModelSerializer):
    # receber "sensor_id" no payload e mapear para o FK "sensor"
    sensor_id = serializers.PrimaryKeyRelatedField(
        queryset=Sensor.objects.all(), source="sensor"
    )

    class Meta:
        model = Leitura
        fields = ("id", "sensor_id", "valor", "tipo", "data_hora")
        read_only_fields = ("id",)
