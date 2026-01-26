# -*- coding: utf-8 -*-
import sys
import os
import pandas as pd
import numpy as np
from sklearn.preprocessing import LabelEncoder
from sklearn.model_selection import train_test_split, cross_val_score
from sklearn.ensemble import RandomForestClassifier
from xgboost import XGBClassifier
from sklearn.impute import SimpleImputer
from sklearn.metrics import f1_score
from pymongo import MongoClient
from bson import ObjectId
import requests
import warnings
import pickle

warnings.filterwarnings("ignore")

class UrineMLPredictor:
    def __init__(self):
        self.model_path = 'trained_models.pkl'
        self.models = None
        self.rf_f1 = 0
        self.xgb_f1 = 0
        
    def load_or_train_models(self):
        """Load model yang sudah ada atau train model baru"""
        if os.path.exists(self.model_path):
            print("[LOAD] Loading trained models from disk...")
            try:
                with open(self.model_path, 'rb') as f:
                    self.models = pickle.load(f)
                self.rf_f1 = self.models.get('rf_f1', 0.85)
                self.xgb_f1 = self.models.get('xgb_f1', 0.87)
                print("[OK] Models loaded successfully!")
                print(f"      RF F1-Score: {self.rf_f1:.3f}")
                print(f"      XGB F1-Score: {self.xgb_f1:.3f}")
                return self.models
            except Exception as e:
                print(f"[WARN] Failed to load models: {e}")
                print("[TRAIN] Training new models...")
        
        print("[TRAIN] Training new models from Google Sheets dataset...")
        
        # Load data dari Google Sheets
        url = "https://docs.google.com/spreadsheets/d/1R6ehqTzOacp_BCm_XXuvCa180WXsC6x9YLdjg2PuaWU/export?format=csv"
        df = pd.read_csv(url)
        
        print(f"[DATA] Dataset loaded: {df.shape[0]} rows, {df.shape[1]} columns")
        
        # Preprocessing
        df['Berat Jenis'] = df['Berat Jenis'].astype(str).str.replace(',', '.').astype(float)
        
        # Generate NTU jika belum ada
        if 'NTU' not in df.columns:
            def estimate_ntu(kejernihan):
                kej = str(kejernihan).upper()
                if 'JERNIH' in kej or 'BENING' in kej:
                    return np.random.uniform(0, 10)
                elif 'AGAK' in kej or 'SEDIKIT' in kej:
                    return np.random.uniform(10, 30)
                elif 'KERUH' in kej:
                    return np.random.uniform(30, 60)
                else:
                    return np.random.uniform(5, 15)
            df['NTU'] = df['Kejernihan'].apply(estimate_ntu)
            print("[OK] Generated NTU column from Kejernihan")
        
        # Fitur dan target
        X = df[['Warna', 'Kejernihan', 'pH', 'Berat Jenis', 'NTU']].copy()
        y = df['Data Penyakit'].copy()
        
        # Label encoding
        le_warna = LabelEncoder()
        le_kejernihan = LabelEncoder()
        le_penyakit = LabelEncoder()
        
        X['Warna'] = le_warna.fit_transform(X['Warna'].astype(str))
        X['Kejernihan'] = le_kejernihan.fit_transform(X['Kejernihan'].astype(str))
        y = le_penyakit.fit_transform(y.astype(str))
        
        # Imputasi
        imputer = SimpleImputer(strategy="mean")
        X[['pH', 'Berat Jenis', 'NTU']] = imputer.fit_transform(X[['pH', 'Berat Jenis', 'NTU']])
        
        # Train-test split
        X_train, X_test, y_train, y_test = train_test_split(
            X, y, test_size=0.2, random_state=42, stratify=y
        )
        
        # Training Random Forest
        print("[TRAIN] Training Random Forest...")
        rf = RandomForestClassifier(n_estimators=100, random_state=42, max_depth=10)
        rf.fit(X_train, y_train)
        rf_pred = rf.predict(X_test)
        self.rf_f1 = f1_score(y_test, rf_pred, average='weighted', zero_division=0)
        
        # Training XGBoost
        print("[TRAIN] Training XGBoost...")
        xgb = XGBClassifier(use_label_encoder=False, eval_metric='mlogloss', random_state=42, max_depth=6)
        xgb.fit(X_train, y_train)
        xgb_pred = xgb.predict(X_test)
        self.xgb_f1 = f1_score(y_test, xgb_pred, average='weighted', zero_division=0)
        
        print("[OK] Training completed!")
        print(f"     RF F1-Score: {self.rf_f1:.3f}")
        print(f"     XGB F1-Score: {self.xgb_f1:.3f}")
        
        # Save models
        self.models = {
            'rf': rf,
            'xgb': xgb,
            'le_warna': le_warna,
            'le_kejernihan': le_kejernihan,
            'le_penyakit': le_penyakit,
            'imputer': imputer,
            'rf_f1': self.rf_f1,
            'xgb_f1': self.xgb_f1
        }
        
        with open(self.model_path, 'wb') as f:
            pickle.dump(self.models, f)
        print(f"[SAVE] Models saved to {self.model_path}")
        
        return self.models
    
    def tds_to_sg(self, tds):
        """Konversi TDS ke Specific Gravity"""
        if tds < 50:
            return 1.005
        elif tds <= 500:
            return round(1.005 + (tds - 50) * 0.000011, 3)
        elif tds <= 1500:
            return round(1.010 + (tds - 500) * 0.000015, 3)
        else:
            return round(1.025 + (tds - 1500) * 0.000003, 3)
    
    def preprocess_mongo_data(self, mongo_data):
        """Preprocessing data dari MongoDB"""
        # pH
        ph = float(mongo_data.get('ph', 7.0))
        
        # Warna
        warna = mongo_data.get('warnaDasar', '').title()
        if not warna or warna == 'N/A':
            analisis = mongo_data.get('analisis', '').upper()
            if 'KUNING' in analisis:
                warna = 'Kuning'
            elif 'MERAH' in analisis:
                warna = 'Merah'
            elif 'COKLAT' in analisis or 'COKELAT' in analisis:
                warna = 'Coklat'
            elif 'ORANGE' in analisis or 'JINGGA' in analisis:
                warna = 'Orange'
            elif 'BIRU' in analisis:
                warna = 'Biru'
            else:
                warna = 'Kuning'
        
        # Kejernihan dari turbidityLevel
        turbidity_level = mongo_data.get('turbidityLevel', '').upper()
        if 'JERNIH' in turbidity_level:
            kejernihan = 'Jernih'
        elif 'AGAK' in turbidity_level:
            kejernihan = 'Agak Keruh'
        elif 'KERUH' in turbidity_level:
            kejernihan = 'Keruh'
        else:
            kejernihan = 'Jernih'
        
        # NTU
        ntu = float(mongo_data.get('turbidityNTU', 5))
        if ntu == 0:
            if kejernihan == 'Jernih':
                ntu = 5.0
            elif kejernihan == 'Agak Keruh':
                ntu = 20.0
            else:
                ntu = 40.0
        
        # Berat Jenis
        sg = float(mongo_data.get('specificGravity', 0))
        if not sg or sg == 0:
            tds_value = float(mongo_data.get('tds', 0))
            sg = self.tds_to_sg(tds_value)
        
        print("\n[DATA] Preprocessing:")
        print(f"       Warna: {warna}")
        print(f"       Kejernihan: {kejernihan}")
        print(f"       pH: {ph:.2f}")
        print(f"       Berat Jenis: {sg:.3f}")
        print(f"       NTU: {ntu:.1f}")
        
        # Encoding
        le_warna = self.models['le_warna']
        le_kejernihan = self.models['le_kejernihan']
        
        try:
            warna_enc = le_warna.transform([warna])[0]
        except ValueError:
            print(f"[WARN] Warna '{warna}' unknown, using 'Kuning'")
            warna_enc = le_warna.transform(['Kuning'])[0]
        
        try:
            kejernihan_enc = le_kejernihan.transform([kejernihan])[0]
        except ValueError:
            print(f"[WARN] Kejernihan '{kejernihan}' unknown, using 'Jernih'")
            kejernihan_enc = le_kejernihan.transform(['Jernih'])[0]
        
        return [[warna_enc, kejernihan_enc, ph, sg, ntu]]
    
    def get_recommendation(self, disease, confidence):
        """Generate rekomendasi medis"""
        disease_upper = disease.upper()
        
        recommendations = {
            'ISK': 'Segera konsultasi dokter untuk urine kultur dan pemberian antibiotik yang tepat',
            'INFEKSI': 'Konsultasi dokter untuk pemeriksaan lebih lanjut dan terapi antibiotik',
            'BATU': 'Perbanyak minum air (2-3 liter/hari) dan konsultasi urolog untuk USG/CT-Scan',
            'GINJAL': 'Segera konsultasi dokter spesialis urologi untuk evaluasi fungsi ginjal',
            'DIABETES': 'Cek gula darah segera dan konsultasi dokter untuk manajemen diabetes',
            'DEHIDRASI': 'Tingkatkan asupan cairan, minum air minimal 8 gelas per hari',
            'NORMAL': 'Kondisi urine normal, pertahankan hidrasi yang baik dan pola hidup sehat'
        }
        
        for key, rec in recommendations.items():
            if key in disease_upper:
                return rec
        
        return 'Konsultasi dokter untuk evaluasi dan pemeriksaan lebih lanjut'
    
    def get_risk_level(self, disease, confidence):
        """Tentukan risk level"""
        if confidence < 0.5:
            return 'Rendah'
        
        disease_upper = disease.upper()
        
        # High risk conditions
        if any(word in disease_upper for word in ['ISK', 'BATU', 'GINJAL', 'INFEKSI']):
            if confidence > 0.8:
                return 'Tinggi'
            elif confidence > 0.6:
                return 'Sedang'
            else:
                return 'Rendah'
        
        # Normal condition
        if 'NORMAL' in disease_upper:
            return 'Normal'
        
        # Default
        return 'Sedang' if confidence > 0.6 else 'Rendah'
    
    def predict(self, data_id):
        """Main prediction function"""
        try:
            # Load models
            if not self.models:
                self.load_or_train_models()
            
            # Connect MongoDB
            print("\n[CONNECT] Connecting to MongoDB...")
            client = MongoClient("mongodb://localhost:27017/", serverSelectionTimeoutMS=5000)
            db = client["iot_urine"]
            collection = db["sensordataaaa"]
            
            # Get data by ID
            mongo_data = collection.find_one({"_id": ObjectId(data_id)})
            
            if not mongo_data:
                print(f"[ERROR] Data with ID {data_id} not found in database")
                return False
            
            print(f"[OK] Data found: {mongo_data.get('timestamp')}")
            
            # Preprocess data
            X_new = self.preprocess_mongo_data(mongo_data)
            
            # Get models
            rf = self.models['rf']
            xgb = self.models['xgb']
            le_penyakit = self.models['le_penyakit']
            
            # Predict with Random Forest
            rf_pred = rf.predict(X_new)[0]
            rf_proba = rf.predict_proba(X_new)[0]
            rf_confidence = float(np.max(rf_proba))
            rf_label = le_penyakit.inverse_transform([rf_pred])[0]
            
            # Predict with XGBoost
            xgb_pred = xgb.predict(X_new)[0]
            xgb_proba = xgb.predict_proba(X_new)[0]
            xgb_confidence = float(np.max(xgb_proba))
            xgb_label = le_penyakit.inverse_transform([xgb_pred])[0]
            
            print("\n[ML] Model Predictions:")
            print(f"     Random Forest: {rf_label} ({rf_confidence*100:.1f}%)")
            print(f"     XGBoost: {xgb_label} ({xgb_confidence*100:.1f}%)")
            
            # Ensemble prediction (weighted average)
            rf_weight = self.rf_f1
            xgb_weight = self.xgb_f1
            
            weighted_proba = (rf_weight * rf_proba + xgb_weight * xgb_proba) / (rf_weight + xgb_weight)
            ensemble_pred = np.argmax(weighted_proba)
            ensemble_label = le_penyakit.inverse_transform([ensemble_pred])[0]
            ensemble_confidence = float(np.max(weighted_proba))
            
            print(f"\n[RESULT] Ensemble: {ensemble_label} ({ensemble_confidence*100:.1f}%)")
            
            # Get recommendation and risk
            recommendation = self.get_recommendation(ensemble_label, ensemble_confidence)
            risk_level = self.get_risk_level(ensemble_label, ensemble_confidence)
            
            print(f"         Risk Level: {risk_level}")
            print(f"         Recommendation: {recommendation[:50]}...")
            
            # Prepare update data
            update_data = {
                'disease': ensemble_label,
                'confidence': ensemble_confidence,
                'rfPrediction': rf_label,
                'rfConfidence': rf_confidence,
                'xgbPrediction': xgb_label,
                'xgbConfidence': xgb_confidence,
                'recommendation': recommendation,
                'riskLevel': risk_level
            }
            
            # Update MongoDB via API
            print("\n[SEND] Sending prediction to backend API...")
            response = requests.post(
                f'http://localhost:3000/api/prediction/{data_id}',
                json=update_data,
                timeout=10
            )
            
            if response.status_code == 200:
                print("[OK] Prediction saved successfully to database!")
                print("\n" + "="*70)
                print("PREDICTION COMPLETE")
                print("="*70)
                print(f"   Disease: {ensemble_label}")
                print(f"   Confidence: {ensemble_confidence*100:.1f}%")
                print(f"   Risk: {risk_level}")
                print("="*70 + "\n")
                return True
            else:
                print(f"[ERROR] Failed to save prediction: {response.status_code}")
                print(f"        Response: {response.text}")
                return False
                
        except Exception as e:
            print(f"[ERROR] Prediction Error: {str(e)}")
            import traceback
            traceback.print_exc()
            return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("[ERROR] Usage: python ml_predict.py <data_id>")
        print("        Example: python ml_predict.py 507f1f77bcf86cd799439011")
        sys.exit(1)
    
    data_id = sys.argv[1]
    
    print("="*70)
    print("IoT Urine Analyzer - ML Prediction System")
    print("="*70)
    print(f"Processing Data ID: {data_id}\n")
    
    predictor = UrineMLPredictor()
    success = predictor.predict(data_id)
    
    if success:
        print("[OK] Process completed successfully!")
        sys.exit(0)
    else:
        print("[ERROR] Process failed!")
        sys.exit(1)